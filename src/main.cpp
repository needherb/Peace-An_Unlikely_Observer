#include <array>
#include <filesystem>
#include "PCH.h"
#include "AdmissionDiagnostics.h"
#include "DiagnosticTimeline.h"
#include "Retaliation.h"
#include "PlayerMount.h"
#include <mutex>
#include <vector>
#include <chrono>
#include <deque>
#include <unordered_map>

namespace
{
    class CombatCleanup final : public RE::BSTEventSink<RE::TESCombatEvent>,
                                public RE::BSTEventSink<RE::TESHitEvent>,
                                public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static CombatCleanup& Get()
        {
            static CombatCleanup instance;
            return instance;
        }

        void Register()
        {
            if (registered_) {
                return;
            }
            auto* source = RE::ScriptEventSourceHolder::GetSingleton();
            if (!source) {
                SKSE::log::error("Combat event source unavailable");
                return;
            }
            source->AddEventSink<RE::TESCombatEvent>(this);
            source->AddEventSink<RE::TESHitEvent>(this);
            source->AddEventSink<RE::TESContainerChangedEvent>(this);
            registered_ = true;
            SKSE::log::info("Combat cleanup, hit, and inventory diagnostic listeners registered");
        }

        void BeginLoad()
        {
            AdmissionDiagnostics::BeginLoad();
            Retaliation::Reset();
            std::scoped_lock lock(mutex_);
            ++generation_;
            ready_ = false;
            pending_ = false;
            history_.clear();
            pendingSnapshots_ = 0;
        }

        void GameReady()
        {
            AdmissionDiagnostics::GameReady();
            {
                std::scoped_lock lock(mutex_);
                ready_ = true;
            }
            Queue();
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* event,
            RE::BSTEventSource<RE::TESCombatEvent>*) override
        {
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            {
                std::scoped_lock lock(mutex_);
                if (!ready_) return RE::BSEventNotifyControl::kContinue;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (event->newState == RE::ACTOR_COMBAT_STATE::kNone) {
                if (auto* actor = event->actor ? event->actor->As<RE::Actor>() : nullptr) {
                    Retaliation::End(actor);
                }
            }
            const bool involvesPlayer = player &&
                (event->actor.get() == player || event->targetActor.get() == player);
            const bool involvesMount = player &&
                (PlayerMount::IsProtected(event->actor ? event->actor->As<RE::Actor>() : nullptr) ||
                 PlayerMount::IsProtected(event->targetActor ? event->targetActor->As<RE::Actor>() : nullptr));
            const bool nearby = player && player->GetParentCell() &&
                ((event->actor && event->actor->GetParentCell() == player->GetParentCell()) ||
                 (event->targetActor && event->targetActor->GetParentCell() == player->GetParentCell()));
            if (involvesPlayer || involvesMount || nearby) {
                {
                    std::scoped_lock lock(mutex_);
                    if (!ready_ || running_) {
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }
                SKSE::log::debug("Combat transition: actor={:08X}, target={:08X}, state={}, involvesPlayer={}",
                    event->actor ? event->actor->GetFormID() : 0,
                    event->targetActor ? event->targetActor->GetFormID() : 0,
                    event->newState.underlying(), involvesPlayer);
                if (auto* actor = event->actor ? event->actor->As<RE::Actor>() : nullptr) {
                    QueueSnapshot(actor->GetHandle(), "combat-transition");
                }
                if ((involvesPlayer || involvesMount) && (event->newState == RE::ACTOR_COMBAT_STATE::kCombat ||
                    event->newState == RE::ACTOR_COMBAT_STATE::kSearching)) {
                    Queue();
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* event,
            RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            {
                std::scoped_lock lock(mutex_);
                if (!ready_) return RE::BSEventNotifyControl::kContinue;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || (event->target.get() != player && event->cause.get() != player)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto stamp = DiagnosticTimeline::Capture();
            if (event->cause.get() == player) {
                if (auto* actor = event->target ? event->target->As<RE::Actor>() : nullptr) {
                    Retaliation::Grant(actor, "player-hit");
                    QueueSnapshot(actor->GetHandle(), "player-caused-hit");
                }
            }
            if (event->target.get() == player) {
                if (auto* actor = event->cause ? event->cause->As<RE::Actor>() : nullptr) {
                    QueueSnapshot(actor->GetHandle(), "player-hit");
                }
            }
            std::scoped_lock lock(mutex_);
            if (!ready_) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto causeID = event->cause ? event->cause->GetFormID() : 0;
            const auto targetID = event->target ? event->target->GetFormID() : 0;
            SKSE::log::debug("Hit: cause={:08X}, target={:08X}, source={:08X}, projectile={:08X}, flags={}, playerCaused={}, seq={}, elapsedUS={}, generation={}, thread={}",
                causeID, targetID, event->source, event->projectile,
                event->flags.underlying(), event->cause.get() == player, stamp.sequence, stamp.elapsedUS,
                generation_, REX::W32::GetCurrentThreadId());
            // Hit permission is established before deferred cleanup snapshots.
            if (event->cause.get() == player && event->target && event->target->As<RE::Actor>()) {
                if (history_.contains(targetID) || history_.size() < 1024) {
                    auto& history = history_[targetID];
                    history.handle = event->target->As<RE::Actor>()->GetHandle();
                    history.lastPlayerHit = Clock::now();
                    history.lastActivity = history.lastPlayerHit;
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || (event->oldContainer != 0x14 && event->newContainer != 0x14)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const auto stamp = DiagnosticTimeline::Capture();
            std::scoped_lock lock(mutex_);
            if (ready_) {
                SKSE::log::debug("Player inventory transfer: seq={}, elapsedUS={}, generation={}, thread={}, from={:08X}, to={:08X}, item={:08X}, count={}, uniqueID={}, theftConfirmed=false",
                    stamp.sequence, stamp.elapsedUS, generation_, REX::W32::GetCurrentThreadId(),
                    event->oldContainer, event->newContainer, event->baseObj, event->itemCount, event->uniqueID);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        // Snapshot on the main thread, not from the event-dispatch thread.
        // It describes state at task execution, not exact state at the hit.
        void QueueSnapshot(RE::ActorHandle handle, const char* reason)
        {
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                return;
            }
            std::scoped_lock lock(mutex_);
            if (!ready_ || pendingSnapshots_ >= 64) {
                return;
            }
            ++pendingSnapshots_;
            const auto generation = generation_;
            tasks->AddTask([this, handle, reason, generation]() {
                {
                    std::scoped_lock snapshotLock(mutex_);
                    if (!ready_ || generation != generation_) {
                        return;
                    }
                    --pendingSnapshots_;
                }
                if (auto actor = handle.get()) {
                    LogSnapshot(actor.get(), reason);
                }
            });
        }

        static void LogSnapshot(RE::Actor* actor, const char* reason)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto current = actor->GetActorRuntimeData().currentCombatTarget.get();
            auto* controller = actor->GetActorRuntimeData().combatController;
            auto controllerTarget = controller ? controller->targetHandle.get() : RE::NiPointer<RE::Actor>{};
            auto previousTarget = controller ? controller->previousTargetHandle.get() : RE::NiPointer<RE::Actor>{};
            auto* faction = actor->GetCrimeFaction();
            SKSE::log::debug("Actor snapshot: reason={}, id={:08X}, name={}, inCombat={}, actorTarget={:08X}, controllerTarget={:08X}, previousTarget={:08X}, hasController={}, group=0x{:X}, targetsPlayer={}, angryWithPlayer={}, trespassing={}, crimeFaction={:08X}, playerBounty={}",
                reason, actor->GetFormID(), actor->GetName(), actor->IsInCombat(),
                current ? current->GetFormID() : 0,
                controllerTarget ? controllerTarget->GetFormID() : 0,
                previousTarget ? previousTarget->GetFormID() : 0,
                controller != nullptr, controller ? reinterpret_cast<std::uintptr_t>(controller->combatGroup) : 0,
                player && actor->IsCombatTarget(player), actor->IsAngryWithPlayer(), actor->IsTrespassing(),
                faction ? faction->GetFormID() : 0, player && faction ? player->GetCrimeGoldValue(faction) : 0);
        }

        void Queue()
        {
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                return;
            }
            std::scoped_lock lock(mutex_);
            if (!ready_ || pending_ || running_) {
                return;
            }
            pending_ = true;
            const auto generation = generation_;
            tasks->AddTask([this, generation]() { Run(generation); });
            SKSE::log::debug("Player combat cleanup queued");
        }

        void Run(std::uint64_t generation)
        {
            {
                std::scoped_lock lock(mutex_);
                if (!ready_ || generation != generation_) {
                    return;
                }
                pending_ = false;
                running_ = true;
            }
            // Reset the reentrancy guard even if an allocation or log operation throws.
            struct Guard
            {
                CombatCleanup& owner;
                ~Guard()
                {
                    std::scoped_lock lock(owner.mutex_);
                    owner.running_ = false;
                }
            } guard{ *this };

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* processes = RE::ProcessLists::GetSingleton();
            if (!player || !processes) {
                SKSE::log::warn("Cleanup skipped: player or process lists unavailable");
                return;
            }
            auto protectedMount = PlayerMount::GetProtectedMount();
            std::vector<RE::ActorHandle> opponents;
            processes->ForAllActors([&](RE::Actor* actor) {
                auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
                auto* controller = actor->GetActorRuntimeData().combatController;
                auto controllerTarget = controller ? controller->targetHandle.get() : RE::NiPointer<RE::Actor>{};
                const bool mountFight = (PlayerMount::IsProtected(actor) && actor->IsInCombat()) ||
                    PlayerMount::IsProtected(target.get()) || PlayerMount::IsProtected(controllerTarget.get()) ||
                    (protectedMount && actor->IsCombatTarget(protectedMount.get()));
                if (actor != player && mountFight) {
                    opponents.push_back(actor->GetHandle());
                }
                if (actor != player && !Retaliation::Allowed(actor) &&
                    (actor->IsCombatTarget(player) || player->IsCombatTarget(actor))) {
                    const auto handle = actor->GetHandle();
                    if (std::find(opponents.begin(), opponents.end(), handle) == opponents.end()) {
                        opponents.push_back(handle);
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            // Collect first: StopCombat may change the process/combat lists and emit events.
            std::size_t stopped = 0;
            for (const auto& handle : opponents) {
                if (auto actor = handle.get()) {
                    const bool preserveRetaliation = !PlayerMount::IsProtected(actor.get()) && Retaliation::Allowed(actor.get());
                    RecordCleanup(actor.get());
                    LogSnapshot(actor.get(), "before-stop");
                    actor->StopCombat();
                    // StopCombat emits combat-end synchronously; retain permission to
                    // target the player when clearing a permitted attacker's horse fight.
                    if (preserveRetaliation) Retaliation::Grant(actor.get(), "mount-cleanup");
                    LogSnapshot(actor.get(), "after-stop");
                    ++stopped;
                }
            }
            const bool playerWasInCombat = player->IsInCombat();
            bool permittedFight{};
            processes->ForAllActors([&](RE::Actor* actor) {
                if (actor != player && Retaliation::Allowed(actor) &&
                    (actor->IsCombatTarget(player) || player->IsCombatTarget(actor))) permittedFight = true;
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (!permittedFight) player->StopCombat();
            SKSE::log::debug("Cleanup complete: opponents={}, playerWasInCombat={}",
                stopped, playerWasInCombat);
        }

        using Clock = std::chrono::steady_clock;
        struct History
        {
            RE::ActorHandle handle;
            std::deque<Clock::time_point> cleanups;
            Clock::time_point lastPlayerHit{};
            Clock::time_point lastActivity{};
            bool candidateLogged{ false };
        };

        void RecordCleanup(RE::Actor* actor)
        {
            const auto now = Clock::now();
            std::scoped_lock lock(mutex_);
            // Bound session diagnostics and remove stale/unloaded references.
            std::erase_if(history_, [&](const auto& entry) {
                return !entry.second.handle.get() ||
                    now - entry.second.lastActivity > std::chrono::seconds(30);
            });
            const auto id = actor->GetFormID();
            if (!history_.contains(id) && history_.size() >= 1024) {
                history_.erase(history_.begin());
            }
            auto& history = history_[id];
            history.handle = actor->GetHandle();
            history.lastActivity = now;
            while (!history.cleanups.empty() && now - history.cleanups.front() > std::chrono::seconds(5)) {
                history.cleanups.pop_front();
            }
            if (history.cleanups.empty()) {
                history.candidateLogged = false;
            }
            history.cleanups.push_back(now);
            // Five stops in five seconds is a diagnostic threshold, not an aggression value.
            if (history.cleanups.size() > 5) {
                history.cleanups.pop_front();
            }
            SKSE::log::debug("Stopping actor: id={:08X}, name={}, stopsIn5s={}, targetsPlayer={}",
                id, actor->GetName(), history.cleanups.size(), actor->IsCombatTarget(RE::PlayerCharacter::GetSingleton()));
            if (history.cleanups.size() >= 5 && !history.candidateLogged) {
                history.candidateLogged = true;
                SKSE::log::debug("Fallback candidate: actor={:08X}, repeatedCleanup=true, recentPlayerHit={}, intervention=none",
                    id, history.lastPlayerHit != Clock::time_point{} &&
                        now - history.lastPlayerHit <= std::chrono::seconds(5));
            }
        }

        std::mutex mutex_;
        std::uint64_t generation_{ 0 };
        bool ready_{ false };
        bool pending_{ false };
        bool running_{ false };
        bool registered_{ false };
        std::size_t pendingSnapshots_{ 0 };
        std::unordered_map<RE::FormID, History> history_;
    };

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            CombatCleanup::Get().Register();
            SKSE::log::info("Game data loaded; player combat cleanup ready");
            break;
        case SKSE::MessagingInterface::kNewGame:
            SKSE::log::info("New game started");
            CombatCleanup::Get().BeginLoad();
            CombatCleanup::Get().GameReady();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            CombatCleanup::Get().BeginLoad();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if (message->data) {
                SKSE::log::info("Save loaded successfully");
                CombatCleanup::Get().GameReady();
            } else {
                SKSE::log::warn("Save load failed; cleanup remains suspended");
            }
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    auto logPath = SKSE::log::log_directory();
    if (!logPath) {
        return false;
    }
    *logPath /= "PeaceSkyrim.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath->string(), true);
    auto logger = std::make_shared<spdlog::logger>("PeaceSkyrim", std::move(sink));
    spdlog::set_default_logger(std::move(logger));
    const auto iniPath = std::filesystem::absolute("Data/SKSE/Plugins/PeaceSkyrim.ini");
    std::array<wchar_t, 32> debugValue{};
    REX::W32::GetPrivateProfileStringW(L"Logging", L"Debug", L"false",
        debugValue.data(), static_cast<std::uint32_t>(debugValue.size()), iniPath.c_str());
    const std::wstring_view value(debugValue.data());
    const bool debugLogging = value == L"1" || _wcsicmp(debugValue.data(), L"true") == 0;
    spdlog::set_level(debugLogging ? spdlog::level::debug : spdlog::level::info);
    spdlog::flush_on(debugLogging ? spdlog::level::debug : spdlog::level::info);
    SKSE::log::info("Logging mode: {}", debugLogging ? "debug" : "reduced");

    SKSE::Init(skse, { .log = false, .trampoline = true, .trampolineSize = 256 });
    if (!AdmissionDiagnostics::Install()) {
        return false;
    }
    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("Unable to register SKSE message listener");
        return false;
    }
    Retaliation::InstallSerialization();
    SKSE::log::info("PeaceSkyrim 0.11.2 loaded; retained mount protection, vanilla-duration retaliation, consequence-free lockpicking/trespass");
    return true;
}
