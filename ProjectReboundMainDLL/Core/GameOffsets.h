#pragma once

#include <cstdint>

// Forward declarations for globals owned by Entry.cpp
class LibReplicate;
class ExternalCommandPipe;
extern uintptr_t BaseAddress;
extern LibReplicate* libReplicate;    // was static in original, but extern needed by other modules
extern ExternalCommandPipe* g_CmdFramework;

namespace GameOffsets
{
    inline void* Resolve(uintptr_t baseAddress, uintptr_t offset)
    {
        return reinterpret_cast<void*>(baseAddress + offset);
    }

    namespace Memory
    {
        constexpr uintptr_t FMemoryInit = 0x18F4350;

        // Boundary dedicated-server flags patched before UWorld becomes available.
        constexpr uintptr_t ServerModeFlag0 = 0x5CE2404;
        constexpr uintptr_t ServerModeFlag1 = 0x5CE2405;
    }

    namespace LibReplicate
    {
        constexpr uintptr_t InitListen = 0x91AEB0;
        constexpr uintptr_t CreateChannel = 0x33A66D0;
        constexpr uintptr_t SetChannelActor = 0x31F44F0;
        constexpr uintptr_t ReplicateActor = 0x31F0070;
        constexpr uintptr_t FMemoryMalloc = 0x18F1810;
        constexpr uintptr_t FMemoryFree = 0x18E5490;
        constexpr uintptr_t OrigNotifyControlMessage = 0x36CDCE0;
        constexpr uintptr_t CreateNamedNetDriver = 0x366ADB0;
        constexpr uintptr_t ActorChannelClose = 0x31DA270;
        constexpr uintptr_t SetWorld = 0x33DF330;
        constexpr uintptr_t CallPreReplication = 0x2FEFBD0;
        constexpr uintptr_t SendClientAdjustment = 0x3506320;
    }

    namespace Hook
    {
        constexpr uintptr_t NotifyActorDestroyed = 0x33403E0;
        constexpr uintptr_t NotifyAcceptingConnection = 0x36CDC90;
        constexpr uintptr_t NotifyControlMessage = 0x36CDCE0;
        constexpr uintptr_t TickFlush = 0x33E05F0;
        constexpr uintptr_t ProcessEvent = 0x1BCBE40;
        constexpr uintptr_t ObjectNeedsLoad = 0x1B7B710;
        constexpr uintptr_t ActorNeedsLoad = 0x3124E70;
        constexpr uintptr_t OnFireWeapon = 0x1610500;
        constexpr uintptr_t PostLogin = 0x32903B0;
        constexpr uintptr_t IsDedicatedServer = 0x33266F0;
        constexpr uintptr_t IsServer = 0x3326C60;
        constexpr uintptr_t IsStandalone = 0x3326CE0;
        constexpr uintptr_t ClientDeathCrash = 0x16ABE10;
        constexpr uintptr_t FixEquipErrorCode = 0x16DD080;
        constexpr uintptr_t FixSkinErrorCode = 0x16DCEC0;
        constexpr uintptr_t FixBadgeOrnamentErrorCode = 0x16DCD80;
    }

    namespace ReturnAddress
    {
        constexpr uintptr_t OnFireWeaponAllowedCaller = 0x1608B31;
    }
}
