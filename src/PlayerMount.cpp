#include "PCH.h"
#include "PlayerMount.h"
#include <mutex>

namespace {
    std::mutex mountMutex;
    RE::ActorHandle rememberedMount;
    RE::FormID rememberedIdentity{};
}

void PlayerMount::Remember(RE::ActorHandle mount)
{
    // Vanilla may clear this identity when a stolen horse returns home.
    // Only riding another mount replaces our retained identity.
    if (auto actor = mount.get()) {
        bool changed{};
        {
            std::scoped_lock lock(mountMutex);
            changed = rememberedIdentity != actor->GetFormID();
            rememberedMount = mount;
            rememberedIdentity = actor->GetFormID();
        }
        if (changed) SKSE::log::debug("Protected mount remembered: actor={:08X}, ownershipRequired=false", actor->GetFormID());
    }
}
void PlayerMount::Reset()
{
    std::scoped_lock lock(mountMutex);
    rememberedMount.reset();
    rememberedIdentity = 0;
}
RE::FormID PlayerMount::SavedIdentity()
{
    GetProtectedMount();
    std::scoped_lock lock(mountMutex);
    return rememberedIdentity;
}
void PlayerMount::Restore(RE::FormID identity)
{
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(identity);
    std::scoped_lock lock(mountMutex);
    rememberedIdentity = identity;
    rememberedMount = actor ? actor->GetHandle() : RE::ActorHandle{};
}
RE::NiPointer<RE::Actor> PlayerMount::GetProtectedMount()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return {};
    RE::NiPointer<RE::Actor> current;
    if (player->GetMount(current) && current) {
        Remember(current->GetHandle());
        return current;
    }
    RE::ActorHandle retained;
    RE::FormID identity{};
    {
        std::scoped_lock lock(mountMutex);
        retained = rememberedMount;
        identity = rememberedIdentity;
    }
    if (identity) {
        if (auto actor = retained.get()) return actor;
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(identity)) return RE::NiPointer<RE::Actor>{ actor };
        return {};
    }
    const auto last = player->GetInfoRuntimeData().lastRiddenMount;
    Remember(last);
    return last.get();
}
bool PlayerMount::IsProtected(RE::Actor* actor)
{
    if (!actor || actor == RE::PlayerCharacter::GetSingleton()) return false;
    auto mount = GetProtectedMount();
    return mount.get() == actor;
}
