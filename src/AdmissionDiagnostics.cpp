#include "PCH.h"
#include "AdmissionDiagnostics.h"
#include "DiagnosticTimeline.h"
#include "Retaliation.h"
#include "PlayerMount.h"
#include "MountSetterABI.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <intrin.h>
#include <mutex>

namespace
{
    using AddTarget = bool (*)(RE::CombatGroup*, RE::Actor*);
    AddTarget original{};
    using ModCrimeGold = void (*)(RE::Actor*, RE::TESFaction*, bool, std::int32_t);
    ModCrimeGold originalModCrimeGold{};
    using Alarm = void (*)(RE::Actor*, RE::Crime*, std::uint32_t, bool, RE::Actor*);
    using PendingCrime = void (*)(RE::AIProcess*, RE::Crime*);
    using Warning = void (*)(RE::Actor*);
    Alarm originalAlarm{};
    PendingCrime originalPendingCrime{};
    Warning originalWarning{};
    using StartCombat = bool (*)(RE::Actor*, RE::Actor*, RE::CombatGroup*);
    using Trespass = void (*)(RE::Actor*, RE::TESObjectREFR*, RE::TESForm*, std::int32_t);
    using LockCrime = bool (*)(RE::LockpickingMenu*);
    StartCombat originalStartCombat{};
    using SetMount = MountSetterABI<RE::PlayerCharacter, RE::ActorHandle>;
    SetMount originalSetMount{};
    Trespass originalTrespass{};
    LockCrime originalLockCrime{};
    thread_local RE::FormID initiatingActor{};
    constexpr std::array<std::uint8_t, 8> startBytes{ 0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x54 };
    constexpr std::array<std::uint8_t, 8> trespassBytes{ 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55 };
    constexpr std::array<std::uint8_t, 9> lockBytes{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    constexpr std::array<std::uint8_t, 11> alarmBytes{
        0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x44, 0x89, 0x40, 0x18 };
    constexpr std::array<std::uint8_t, 9> warningBytes{
        0x48, 0x8B, 0xC4, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60 };
    constexpr std::array<std::uint8_t, 9> pendingBytes{
        0x48, 0x8B, 0x41, 0x10, 0x48, 0x85, 0xC0, 0x74, 0x07 };
    constexpr std::uintptr_t targetRVA = 0x803A10;
    // MOV RAX,RSP; PUSH RBP; PUSH RSI; PUSH RDI. No relative operands.
    constexpr std::array<std::uint8_t, 10> expected{
        0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57 };
    struct Record
    {
        std::uint64_t sequence;
        std::int64_t elapsedUS;
        std::uintptr_t group;
        std::uintptr_t callerRVA;
        std::uint32_t thread;
        bool accepted;
        bool crime{ false };
        RE::FormID faction{};
        bool violent{};
        std::int32_t amount{};
        DiagnosticTimeline::Stamp completed{};
        enum class Kind { Admission, Bounty, Alarm, Pending, Warning } kind{};
        std::uint32_t crimeType{ 0xFFFFFFFF };
        std::uint32_t offenderHandle{};
        std::uint32_t victimHandle{};
        std::uint32_t crimeID{};
        RE::FormID actorID{};
        std::uintptr_t process{};
        bool hasHighProcess{};
        bool reportedFlag{};
        bool originalCalled{};
    };
    std::mutex mutex;
    std::array<Record, 256> records{};
    std::size_t count{};
    std::atomic<std::uint64_t> generation{};
    bool pending{};
    std::atomic<RE::Actor*> player{};
    std::atomic<std::uint32_t> playerHandle{};
    std::atomic<std::uint64_t> dropped{};

    void Drain(std::uint64_t epoch)
    {
        std::array<Record, 256> batch{};
        std::size_t size{};
        std::uint64_t lost{};
        {
            std::scoped_lock lock(mutex);
            if (epoch != generation || !player.load()) {
                return;
            }
            size = count;
            std::copy_n(records.begin(), size, batch.begin());
            count = 0;
            pending = false;
            lost = dropped.exchange(0);
        }
        for (std::size_t i = 0; i < size; ++i) {
            const auto& r = batch[i];
            if (r.kind == Record::Kind::Alarm || r.kind == Record::Kind::Pending || r.kind == Record::Kind::Warning) {
                const char* phase = r.kind == Record::Kind::Alarm ? "alarm-entry" :
                    r.kind == Record::Kind::Pending ? "pending-assignment" : "steal-warning-entry";
                SKSE::log::debug("Classified crime: phase={}, seq={}, generation={}, elapsedUS={}, crimeID={}, type={}, offenderHandle={:08X}, victimHandle={:08X}, actor={:08X}, process=0x{:X}, hasHighProcess={}, faction={:08X}, amount={}, recordFlag4C={}, callerRVA=0x{:X}, thread={}, mode={}, capturedBeforeOriginal=true",
                    phase, r.sequence, epoch, r.elapsedUS, r.crimeID, r.crimeType, r.offenderHandle,
                    r.victimHandle, r.actorID, r.process, r.hasHighProcess, r.faction, r.amount,
                    r.reportedFlag, r.callerRVA, r.thread, r.crimeType == 2 ? "suppress-trespass" : "observe");
                continue;
            }
            if (r.crime) {
                SKSE::log::debug("Bounty modification: seq={}, generation={}, elapsedUS={}, completedSeq={}, completedUS={}, faction={:08X}, violent={}, delta={}, callerRVA=0x{:X}, thread={}, originalCalled=true",
                    r.sequence, epoch, r.elapsedUS, r.completed.sequence, r.completed.elapsedUS,
                    r.faction, r.violent, r.amount, r.callerRVA, r.thread);
                continue;
            }
            SKSE::log::debug("Native admission: seq={}, generation={}, elapsedUS={}, group=0x{:X}, target=00000014, callerRVA=0x{:X}, thread={}, accepted={}, originalCalled={}, mode={}",
                r.sequence, epoch, r.elapsedUS, r.group, r.callerRVA, r.thread, r.accepted,
                r.originalCalled, r.originalCalled ? "vanilla-retaliation" : "reject-player");
        }
        if (lost) {
            SKSE::log::warn("Native diagnostic records dropped: generation={}, count={}", epoch, lost);
        }
    }

    // No group traversal, handle resolution, logging, or combat calls in this observer.
    // The record contains numeric identities only; no engine pointer survives the call.
    void Observe(Record record, RE::Actor* observedPlayer, std::uint64_t epoch) noexcept
    {
        try {
            std::unique_lock lock(mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                ++dropped;
                return;
            }
            if (generation.load() != epoch || player.load() != observedPlayer) {
                return;
            }
            if (count == records.size()) {
                ++dropped;
                return;
            }
            records[count++] = record;
            if (!pending) {
                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    return;
                }
                pending = true;
                try {
                    tasks->AddTask([epoch]() { Drain(epoch); });
                } catch (...) {
                    pending = false;
                    ++dropped;
                }
            }
        } catch (...) {
            ++dropped;
        }
    }

    bool Hook(RE::CombatGroup* group, RE::Actor* target)
    {
        // Mount protection never inherits a group's player-retaliation exception.
        if (player.load() && PlayerMount::IsProtected(target)) return false;
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto epoch = generation.load();
        auto* observedPlayer = player.load();
        const bool relevant = observedPlayer && target == observedPlayer;
        const auto stamp = relevant ? DiagnosticTimeline::Capture() : DiagnosticTimeline::Stamp{};
        // Reject before vanilla's already-present check and all admission side effects.
        // Other targets and calls while loading retain their original behavior.
        const bool permitted = relevant && group && Retaliation::Admission(group->groupID, initiatingActor);
        const bool accepted = !relevant || permitted ? original(group, target) : false;
        if (relevant) {
            Record r{ stamp.sequence, stamp.elapsedUS, reinterpret_cast<std::uintptr_t>(group),
                caller - REL::Module::get().base(), REX::W32::GetCurrentThreadId(), accepted };
            r.originalCalled = permitted;
            Observe(r, observedPlayer, epoch);
        }
        return accepted;
    }

    bool StartHook(RE::Actor* actor, RE::Actor* target, RE::CombatGroup* group)
    {
        // Also stop the mount initiating combat and becoming an enemy itself.
        if (player.load() && (PlayerMount::IsProtected(actor) || PlayerMount::IsProtected(target))) {
            return false;
        }
        const auto previous = initiatingActor;
        struct Restore { RE::FormID saved; ~Restore() { initiatingActor = saved; } } restore{ previous };
        initiatingActor = player.load() && target == player.load() && Retaliation::Allowed(actor)
            ? actor->GetFormID() : 0;
        // Group-authorized actors get individual permission before vanilla starts,
        // allowing serialization and future split-group retaliation.
        if (initiatingActor) Retaliation::Grant(actor, "authorized-combat-group");
        return originalStartCombat(actor, target, group);
    }

    void TrespassHook(RE::Actor* actor, RE::TESObjectREFR* reference, RE::TESForm* owner, std::int32_t crime)
    {
        if (player.load() && actor == player.load()) {
            SKSE::log::debug("Trespass reporting suppressed: reference={:08X}, existingCrime={}, callerRVA=0x{:X}",
                reference ? reference->GetFormID() : 0, crime,
                reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base());
            return;
        }
        originalTrespass(actor, reference, owner, crime);
    }

    void SetMountHook(RE::PlayerCharacter* actor, const RE::ActorHandle& mount)
    {
        if (actor == player.load()) PlayerMount::Remember(mount);
        originalSetMount(actor, mount);
    }
    static_assert(std::is_same_v<decltype(&SetMountHook), SetMount>);
    static_assert(sizeof(RE::ActorHandle) == sizeof(std::uint32_t));

    bool LockHook(RE::LockpickingMenu* menu)
    {
        if (player.load()) {
            menu->GetRuntimeData().isLockpickingCrime = false;
            SKSE::log::debug("Lockpicking crime check suppressed: callerRVA=0x{:X}",
                reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base());
            return false;
        }
        return originalLockCrime(menu);
    }

    void CrimeHook(RE::Actor* actor, RE::TESFaction* faction, bool violent, std::int32_t amount)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto epoch = generation.load();
        auto* observedPlayer = player.load();
        const bool relevant = observedPlayer && actor == observedPlayer;
        const auto stamp = relevant ? DiagnosticTimeline::Capture() : DiagnosticTimeline::Stamp{};
        const auto factionID = relevant && faction ? faction->GetFormID() : 0;
        originalModCrimeGold(actor, faction, violent, amount);
        if (relevant) {
            Record r{ stamp.sequence, stamp.elapsedUS, 0, caller - REL::Module::get().base(),
                REX::W32::GetCurrentThreadId(), false };
            r.crime = true;
            r.faction = factionID;
            r.violent = violent;
            r.amount = amount;
            r.completed = DiagnosticTimeline::Capture();
            Observe(r, observedPlayer, epoch);
        }
    }

    template <class T>
    T ReadCrime(const RE::Crime* crime, std::size_t offset)
    {
        T value{};
        std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(crime) + offset, sizeof(T));
        return value;
    }

    // Runtime-specific scalar fields established by the supplied 1.6.1170 listings.
    // CommonLib leaves these fields unnamed (and labels +0x58 as a pointer).
    Record CrimeRecord(RE::Crime* crime, Record::Kind kind, std::uintptr_t caller)
    {
        const auto stamp = DiagnosticTimeline::Capture();
        Record r{ stamp.sequence, stamp.elapsedUS, 0, caller - REL::Module::get().base(),
            REX::W32::GetCurrentThreadId(), false };
        r.kind = kind;
        if (crime) {
            r.crimeType = ReadCrime<std::uint32_t>(crime, 0x04);
            r.victimHandle = ReadCrime<std::uint32_t>(crime, 0x08);
            r.offenderHandle = ReadCrime<std::uint32_t>(crime, 0x0C);
            r.crimeID = ReadCrime<std::uint32_t>(crime, 0x48);
            r.reportedFlag = ReadCrime<std::uint8_t>(crime, 0x4C) != 0;
            r.amount = ReadCrime<std::int32_t>(crime, 0x58);
            auto* faction = ReadCrime<RE::TESFaction*>(crime, 0x60);
            r.faction = faction ? faction->GetFormID() : 0;
        }
        return r;
    }

    void AlarmHook(RE::Actor* actor, RE::Crime* crime, std::uint32_t arg3, bool arg4, RE::Actor* target)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto epoch = generation.load();
        auto* observedPlayer = player.load();
        if (observedPlayer && (actor == observedPlayer || target == observedPlayer ||
            (crime && ReadCrime<std::uint32_t>(crime, 0x0C) == playerHandle.load()))) {
            auto r = CrimeRecord(crime, Record::Kind::Alarm, caller);
            r.actorID = actor ? actor->GetFormID() : 0;
            auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
            r.process = reinterpret_cast<std::uintptr_t>(process);
            r.hasHighProcess = process && process->high;
            if (r.offenderHandle == playerHandle.load() && (r.crimeType == 0 || r.crimeType == 1)) {
                Retaliation::Grant(actor, r.crimeType == 0 ? "caught-theft-alarm" : "caught-pickpocket-alarm");
            }
            Observe(r, observedPlayer, epoch);
            // Ignore old trespass reactions as well as blocking new reporting.
            // Original bounty modifications and theft alarms remain untouched.
            if (r.offenderHandle == playerHandle.load() && r.crimeType == 2) return;
        }
        originalAlarm(actor, crime, arg3, arg4, target);
    }

    void PendingHook(RE::AIProcess* process, RE::Crime* crime)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto epoch = generation.load();
        auto* observedPlayer = player.load();
        if (observedPlayer && crime && ReadCrime<std::uint32_t>(crime, 0x0C) == playerHandle.load()) {
            auto r = CrimeRecord(crime, Record::Kind::Pending, caller);
            r.process = reinterpret_cast<std::uintptr_t>(process);
            r.hasHighProcess = process && process->high;
            Observe(r, observedPlayer, epoch);
            if (r.crimeType == 2) return;
        }
        originalPendingCrime(process, crime);
    }

    void WarningHook(RE::Actor* actor)
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto epoch = generation.load();
        auto* observedPlayer = player.load();
        if (observedPlayer) {
            auto r = CrimeRecord(nullptr, Record::Kind::Warning, caller);
            r.actorID = actor ? actor->GetFormID() : 0;
            auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
            r.process = reinterpret_cast<std::uintptr_t>(process);
            r.hasHighProcess = process && process->high;
            Observe(r, observedPlayer, epoch);
        }
        originalWarning(actor);
    }

    template <class Function>
    Function InstallObserver(std::uintptr_t rva, std::size_t copied, Function hook)
    {
        const auto address = REL::Module::get().base() + rva;
        auto& trampoline = SKSE::GetTrampoline();
        auto* gateway = static_cast<std::uint8_t*>(trampoline.allocate(copied + 14));
        std::memcpy(gateway, reinterpret_cast<void*>(address), copied);
        constexpr std::array<std::uint8_t, 6> jump{ 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(gateway + copied, jump.data(), jump.size());
        const auto resume = address + copied;
        std::memcpy(gateway + copied + 6, &resume, sizeof(resume));
        REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), gateway, copied + 14);
        trampoline.write_branch<6>(address, hook);
        for (std::size_t i = 6; i < copied; ++i) {
            REL::safe_write<std::uint8_t>(address + i, 0x90);
        }
        REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), reinterpret_cast<void*>(address), copied);
        return reinterpret_cast<Function>(gateway);
    }
}

bool AdmissionDiagnostics::Install()
{
    if (REL::Module::get().version() != REL::Version(1, 6, 1170, 0)) {
        SKSE::log::error("Native diagnostics require Steam runtime 1.6.1170; plugin load rejected");
        return false;
    }
    const auto address = REL::Module::get().base() + targetRVA;
    const auto base = REL::Module::get().base();
    // Validate all sites before mutating any engine code. Copied spans contain
    // complete instructions and no relative operands; the pending JZ stays original.
    if (std::memcmp(reinterpret_cast<void*>(base + 0x67D0B0), alarmBytes.data(), alarmBytes.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + 0x6B6930), startBytes.data(), startBytes.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + 0x6711B0), trespassBytes.data(), trespassBytes.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + 0x939A60), lockBytes.data(), lockBytes.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + 0x6EE750), pendingBytes.data(), pendingBytes.size()) != 0 ||
        std::memcmp(reinterpret_cast<void*>(base + 0x67DCC0), warningBytes.data(), warningBytes.size()) != 0) {
        SKSE::log::error("Classified crime diagnostics prologue mismatch; plugin load rejected before patching");
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(address), expected.data(), expected.size()) != 0) {
        SKSE::log::error("Native diagnostics not installed: AddTarget prologue mismatch (possible hook conflict); plugin load rejected");
        return false;
    }
    auto& trampoline = SKSE::GetTrampoline();
    auto* gateway = static_cast<std::uint8_t*>(trampoline.allocate(20));
    std::memcpy(gateway, expected.data(), 6);
    // RIP-relative absolute jump preserves RAX (the copied prologue's saved RSP).
    constexpr std::array<std::uint8_t, 6> jump{ 0xFF, 0x25, 0, 0, 0, 0 };
    std::memcpy(gateway + 6, jump.data(), jump.size());
    const auto resume = address + 6;
    std::memcpy(gateway + 12, &resume, sizeof(resume));
    REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), gateway, 20);
    original = reinterpret_cast<AddTarget>(gateway);
    trampoline.write_branch<6>(address, Hook);
    REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), reinterpret_cast<void*>(address), 6);
    SKSE::log::info("Native admission hook installed: runtime=1.6.1170, RVA=0x803A10, mode=peace-with-retaliation, capacity=256");
    // CommonLib's SE/AE virtual slot for Actor::ModCrimeGoldValue. Preserve
    // the existing implementation, including another plugin's vtable hook.
    REL::Relocation<std::uintptr_t> playerVTable{ RE::VTABLE_PlayerCharacter[0] };
    originalModCrimeGold = reinterpret_cast<ModCrimeGold>(playerVTable.write_vfunc(0xB6, CrimeHook));
    originalSetMount = reinterpret_cast<SetMount>(playerVTable.write_vfunc(0x113, SetMountHook));
    SKSE::log::info("Player bounty modification diagnostic installed: slot=0xB6, mode=observe");
    originalAlarm = InstallObserver<Alarm>(0x67D0B0, 7, AlarmHook);
    originalPendingCrime = InstallObserver<PendingCrime>(0x6EE750, 7, PendingHook);
    originalWarning = InstallObserver<Warning>(0x67DCC0, 9, WarningHook);
    originalStartCombat = InstallObserver<StartCombat>(0x6B6930, 6, StartHook);
    originalTrespass = InstallObserver<Trespass>(0x6711B0, 6, TrespassHook);
    originalLockCrime = InstallObserver<LockCrime>(0x939A60, 6, LockHook);
    SKSE::log::info("Peace policy hooks installed: startCombatRVA=0x6B6930, trespassRVA=0x6711B0, lockCrimeRVA=0x939A60, retaliation=vanilla-combat-end");
    SKSE::log::info("Classified crime observers installed: alarmRVA=0x67D0B0, pendingRVA=0x6EE750, warningRVA=0x67DCC0, mode=observe");
    return true;
}

void AdmissionDiagnostics::BeginLoad()
{
    std::scoped_lock lock(mutex);
    player.store(nullptr);
    PlayerMount::Reset();
    playerHandle.store(0);
    ++generation;
    count = 0;
    pending = false;
    dropped = 0;
}

void AdmissionDiagnostics::GameReady()
{
    std::scoped_lock lock(mutex);
    auto* readyPlayer = RE::PlayerCharacter::GetSingleton();
    playerHandle.store(readyPlayer ? readyPlayer->GetHandle().native_handle() : 0);
    player.store(readyPlayer);
    SKSE::log::debug("Native admission gate active: generation={}, mainThread={}, mode=peace-with-retaliation", generation.load(), REX::W32::GetCurrentThreadId());
}
