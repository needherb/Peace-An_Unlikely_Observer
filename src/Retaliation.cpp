#include "PCH.h"
#include "Retaliation.h"
#include "RetaliationState.h"
#include "PlayerMount.h"
#include <mutex>
#include <unordered_set>
#include <unordered_map>
namespace
{
    std::mutex policyMutex;
    RetaliationState state;
    constexpr std::uint32_t recordType = 0x52545053; // SPTR
    constexpr std::uint32_t mountRecordType = 0x544E4D50; // PMNT
    void Save(SKSE::SerializationInterface* serialization)
    {
        const auto mount = PlayerMount::SavedIdentity();
        if (mount && !serialization->WriteRecord(mountRecordType, 1, mount)) {
            SKSE::log::error("Unable to save protected mount identity");
        }
        std::scoped_lock lock(policyMutex);
        for (const auto actor : state.actors) {
            if (!serialization->WriteRecord(recordType, 1, actor)) {
                SKSE::log::error("Unable to save retaliation permission");
                break;
            }
        }
    }
    void Load(SKSE::SerializationInterface* serialization)
    {
        Retaliation::Reset();
        PlayerMount::Reset();
        std::uint32_t type{}, version{}, length{};
        while (serialization->GetNextRecordInfo(type, version, length)) {
            if (type == mountRecordType && version == 1 && length == sizeof(RE::FormID)) {
                RE::FormID saved{}, resolved{};
                if (serialization->ReadRecordData(saved) == sizeof(saved) && serialization->ResolveFormID(saved, resolved)) {
                    PlayerMount::Restore(resolved);
                }
                continue;
            }
            if (type != recordType || version != 1 || length != sizeof(RE::FormID)) continue;
            RE::FormID saved{}, resolved{};
            if (serialization->ReadRecordData(saved) == sizeof(saved) && serialization->ResolveFormID(saved, resolved)) {
                std::scoped_lock lock(policyMutex);
                state.Grant(resolved);
            }
        }
        SKSE::log::debug("Retaliation permissions restored from co-save: count={}", state.actors.size());
    }
    void Revert(SKSE::SerializationInterface*) { Retaliation::Reset(); PlayerMount::Reset(); }
}
void Retaliation::InstallSerialization()
{
    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(0x50534B59); // PSKY
    serialization->SetSaveCallback(Save);
    serialization->SetLoadCallback(Load);
    serialization->SetRevertCallback(Revert);
}
void Retaliation::Reset()
{
    std::scoped_lock lock(policyMutex);
    state.Reset();
}
void Retaliation::Grant(RE::Actor* actor, const char* reason)
{
    if (!actor || actor->GetFormID() == 0x14 || PlayerMount::IsProtected(actor)) return;
    bool added{};
    {
        std::scoped_lock lock(policyMutex);
        added = state.Grant(actor->GetFormID());
    }
    if (added) SKSE::log::debug("Retaliation granted: actor={:08X}, reason={}, duration=vanilla-combat", actor->GetFormID(), reason);
}
void Retaliation::End(RE::Actor* actor)
{
    if (!actor) return;
    bool removed{};
    {
        std::scoped_lock lock(policyMutex);
        removed = state.End(actor->GetFormID());
    }
    if (removed) SKSE::log::debug("Retaliation released: actor={:08X}, reason=vanilla-combat-end", actor->GetFormID());
}
bool Retaliation::Allowed(RE::Actor* actor)
{
    if (!actor || PlayerMount::IsProtected(actor)) return false;
    const auto* controller = actor->GetActorRuntimeData().combatController;
    const auto* group = controller ? controller->combatGroup : nullptr;
    std::scoped_lock lock(policyMutex);
    return state.Allowed(actor->GetFormID(), group ? group->groupID : 0);
}
bool Retaliation::Admission(std::uint32_t groupID, RE::FormID initiatingActor)
{
    std::scoped_lock lock(policyMutex);
    return state.Admission(groupID, initiatingActor);
}
