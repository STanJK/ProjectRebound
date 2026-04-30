// Main.cpp
#include <Windows.h>
#include <thread>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>

#include "SDK.hpp"
#include "GameOffsets.h"
#include "Network/NetDriverAccess.h"
#include "SDK/Engine_parameters.hpp"
#include "SDK/ProjectBoundary_parameters.hpp"
#include "safetyhook/safetyhook.hpp"
#include "Libs/json.hpp"
#include "Replication/libreplicate.h"
#include "ServerLogic/LateJoinManager.h"
#include "Communication/CommandFramework.h"

#include "Config/Config.h"
#include "Debug/Debug.h"
#include "ServerLogic/ServerLogic.h"
#include "ClientLogic/ClientLogic.h"
#include "Hooks/Hooks.h"
#include "Network/Network.h"
#include "Utility/Utility.h"

using namespace SDK;
// ======================================================
//  SECTION 3 — GLOBAL VARIABLES (now owned by Main)
// ======================================================

uintptr_t BaseAddress = 0x0;
LibReplicate* libReplicate = nullptr; // was static in original, but extern needed by other modules
static CommandFramework* g_CmdFramework = nullptr;

void OnJoinFromPipe(const std::string& ip, const std::string& token)
{
    (void)token;

    ClientLog("[PIPE] Join request received: " + ip);
    {
        std::lock_guard<std::mutex> lock(MatchIPMutex);
        MatchIP = ip;
    }

    if (UWorld::GetWorld() && UWorld::GetWorld()->OwningGameInstance)
    {
        ConnectToMatch();
    }
    else
    {
        AutoConnectToMatchFromCmdline();
    }
}

// ======================================================
//  SECTION 15 — MAIN THREAD (ENTRY LOGIC)
// ======================================================

void MainThread()
{
    ClientLog("[BOOT] DLL injected, starting...");
    try
    {
        // Calms down the ui font missing panic
        InitMessageBoxHook();

        BaseAddress = (uintptr_t)GetModuleHandleA(nullptr);

        UC::FMemory::Init(GameOffsets::Resolve(BaseAddress, GameOffsets::Memory::FMemoryInit));

        if (std::string(GetCommandLineA()).contains("-server"))
        {
            amServer = true;
        }

        while (!UWorld::GetWorld())
        {
            if (amServer)
            {
                *reinterpret_cast<__int8*>(BaseAddress + GameOffsets::Memory::ServerModeFlag0) = 0;
                *reinterpret_cast<__int8*>(BaseAddress + GameOffsets::Memory::ServerModeFlag1) = 1;
            }

            Sleep(10);
        }

        // DebugLocateSubsystems();
        // DebugDumpSubsystemsToFile();

        if (amServer)
        {
            InitServerHooks();
            Log("[SERVER] Hooks installed.");

            // Wait for world
            Log("[SERVER] Waiting for UWorld...");
            while (!UWorld::GetWorld())
                Sleep(10);
            Log("[SERVER] UWorld is ready.");

            // Initialize LibReplicate exactly like original code
            libReplicate = new LibReplicate(
                LibReplicate::EReplicationMode::Minimal,
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::InitListen),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::CreateChannel),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::SetChannelActor),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::ReplicateActor),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::FMemoryMalloc),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::FMemoryFree),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::OrigNotifyControlMessage),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::CreateNamedNetDriver),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::ActorChannelClose),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::SetWorld),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::CallPreReplication),
                GameOffsets::Resolve(BaseAddress, GameOffsets::LibReplicate::SendClientAdjustment));
            Log("[SERVER] LibReplicate initialized.");

            // Initialize LateJoinManager
            gLateJoinManager = new LateJoinManager(
                DidProcStartMatch,
                PlayerRespawnAllowedMap,
                nullptr // ReportRoomStarted callback — can be wired later
            );
            Log("[SERVER] LateJoinManager initialized.");

            StartServer();

            // Heartbeat thread (game + backend) – now wrapped in Network
            StartHeartbeatThread();
        }
        else
        {
            // We're client
            LoadClientConfig();
            // Initialize client debug log
            if (ClientDebugLogEnabled)
            {
                std::filesystem::create_directory("clientlogs");

                std::string path = "clientlogs/clientlog-" + CurrentTimestamp() + ".txt";
                clientLogFile.open(path, std::ios::app);

                std::cout << "[CLIENT] Debug logging enabled: " << path << std::endl;
            }
            InitDebugConsole();
            EnableUnrealConsole();

            InitClientHook();

            // Disabled LocalURL override; add a GameOffsets constant before restoring this path.
            // auto dump below
            // std::thread(ClientAutoDumpThread).detach();
            // Init Hotkey Check
            // Only start the hotkey thread if the -debug flag is present
            if (std::string(GetCommandLineA()).find("-debug") != std::string::npos)
            {
                std::thread(HotkeyThread).detach();
            }

            InitClientArmory();
            if (!MatchPipeName.empty())
            {
                g_CmdFramework = new CommandFramework();
                g_CmdFramework->SetPipeName(MatchPipeName);
                g_CmdFramework->SetJoinCallback(OnJoinFromPipe);
                g_CmdFramework->SetLogCallback([](const std::string& msg) { ClientLog(msg); });
                g_CmdFramework->Start();
            }

            bool hasInitialMatchTarget = false;
            {
                std::lock_guard<std::mutex> lock(MatchIPMutex);
                hasInitialMatchTarget = !MatchIP.empty();
            }
            if (hasInitialMatchTarget)
            {
                AutoConnectToMatchFromCmdline();
            }
            /*
            Sleep(10 * 1000);

            UCommonActivatableWidget* widget = nullptr;
            reinterpret_cast<UPBMainMenuManager_BP_C*>(getObjectsOfClass(UPBMainMenuManager_BP_C::StaticClass(), false).back())->GetTopMenuWidget(&widget);
            widget->SetVisibility(ESlateVisibility::Hidden);
            widget->DeactivateWidget();

            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open 73.130.167.222", nullptr);
            */

            // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open 127.0.0.1", nullptr);
        }
    }
    catch (...)
    {
        std::cout << "[ERROR] Unhandled exception in MainThread!" << std::endl;
        std::cout << "Press ENTER to exit..." << std::endl;
        std::cin.get();
    }
}

// ======================================================
//  SECTION 16 — DLL ENTRY POINT
// ======================================================

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        std::thread t(MainThread);

        t.detach();
    }

    return TRUE;
}
