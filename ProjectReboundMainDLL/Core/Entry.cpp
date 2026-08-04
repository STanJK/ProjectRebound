#include "Entry.h"
#include "GameOffsets.h"

#include <Windows.h>
#include <thread>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>

#include "../SDK.hpp"
#include "../SDK/Engine_parameters.hpp"
#include "../SDK/ProjectBoundary_parameters.hpp"
#include "../Libs/safetyhook.hpp"
#include "../Libs/json.hpp"

#include "../Logging/LogManager.h"
#include "../Config/Config.h"
#include "../Hooking/HookCore.h"
#include "../Server/Replication.h"
#include "../Server/LateJoin.h"
#include "../Server/RoundManager.h"
#include "../Server/Backend.h"
#include "../Client/AutoConnect.h"
#include "../API/ExternalCommandPipe.h"
#include "../API/APIInternal.h"
#include "../Server/PlayerNaming.h"

using namespace SDK;

// ======================================================
//  Global variables
// ======================================================

uintptr_t BaseAddress = 0x0;
LibReplicate* libReplicate = nullptr; // was static in original, but extern needed by other modules
ExternalCommandPipe* g_CmdFramework = nullptr;
static std::mutex g_CmdFrameworkMutex;

// ======================================================
//  Pipe join callback
// ======================================================

static bool OnJoinFromPipe(const std::string& ip, const std::string& token)
{
    (void)token;

    ClientDebugLog("[PIPE] Join request received: " + ip);
    return QueueConnectToMatch(ip);
}

// Process termination is left to Windows. Explicit DLL unloaders must call
// this outside DllMain so Stop() can join without holding the loader lock.
extern "C" __declspec(dllexport) void ShutdownPayloadCommandFramework()
{
    ExternalCommandPipe* framework = nullptr;
    bool calledFromListener = false;
    {
        std::lock_guard<std::mutex> lock(g_CmdFrameworkMutex);
        if (g_CmdFramework != nullptr && g_CmdFramework->IsListenerThread())
        {
            calledFromListener = true;
        }
        else
        {
            framework = g_CmdFramework;
            g_CmdFramework = nullptr;
        }
    }

    if (calledFromListener)
    {
        ClientDebugLog("[PIPE] Shutdown must be requested by an external owner thread.");
        return;
    }
    if (framework != nullptr)
    {
        framework->Stop();
        delete framework;
    }
}

// ======================================================
//  DllMain
// ======================================================

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        std::thread t(MainThread);
        t.detach();
    }

    return TRUE;
}

// ======================================================
//  MainThread — full server/client initialization
// ======================================================

void MainThread()
{
    ClientDebugLog("[BOOT] DLL injected, starting...");
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
            // ---- Server path ----

            InitServerHooks();
            ServerLog("[SERVER] Hooks installed.");

            // Wait for world
            ServerLog("[SERVER] Waiting for UWorld...");
            while (!UWorld::GetWorld())
                Sleep(10);
            ServerLog("[SERVER] UWorld is ready.");

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
            ServerLog("[SERVER] LibReplicate initialized.");

            // Initialize LateJoinManager
            gLateJoinManager = new LateJoinManager(
                DidProcStartMatch,
                PlayerRespawnAllowedMap,
                nullptr // ReportRoomStarted callback — can be wired later
            );
            ServerLog("[SERVER] LateJoinManager initialized.");

            StartServer();

            // Heartbeat thread (game + backend)
            StartHeartbeatThread();
        }
        else
        {
            // ---- Client path ----

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
                auto framework = std::make_unique<ExternalCommandPipe>();
                framework->SetPipeName(MatchPipeName);
                framework->SetJoinCallback(OnJoinFromPipe);
                framework->SetLogCallback([](const std::string& msg) { ClientDebugLog(msg); });

                if (framework->Start())
                {
                    std::lock_guard<std::mutex> lock(g_CmdFrameworkMutex);
                    g_CmdFramework = framework.release();
                }
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
        ServerLog("[ERROR] Unhandled exception in MainThread!");
        ServerLog("Press ENTER to exit...");
        std::cin.get();
    }
}
