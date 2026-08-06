// Main.cpp
//
// The purpose of this file is to act as the entry point for the payload.
// To improve stability and maintainability the bulk of initialization has been
// moved out of DllMain.  DllMain now only kicks off a worker thread and
// signals shutdown on unload.  Additional helpers have been added to
// safely write to game memory, parse the command line, and manage
// the lifetime of global objects.
// 引入 framework.h 并定义 NOMINMAX；新增 <atomic>, <chrono>, <memory>, <cstring> 等头文件。
// 使用 std::unique_ptr 管理 LibReplicate、CommandFramework 和 LateJoinManager，在线程结束时统一释放。
// 新增 IsArgumentPresent() 用于可靠地检查 -server、-debug 参数。
// 新增 PatchServerModeFlags() 函数用于安全地写入 server 标志位。
// 新增 MainThreadThunk() 封装 MainThread()，用于捕获异常并输出调试信息。
// 将原来的 std::thread(MainThread).detach() 改为 CreateThread，并在 DllMain 中调用 DisableThreadLibraryCalls 避免多线程回调
// 在 MainThread() 中增加了对 g_ShutdownRequested 的检查，使等待 UWorld 和后续循环均能安全退出；在 server 分支和 client 分支中按需求初始化资源；在结束时调用 Stop() 和重置智能指针
// 消除早退导致的资源泄露,引入了 exitEarly 标志，所有早退路径都通过设置此标志退出循环，以确保函数末尾的清理逻辑一定执行，RAII 对象在函数结尾统一 reset
// 统一世界等待逻辑，在等待 UWorld 的循环中增加了对 g_ShutdownRequested 的检查
// 重构服务器和客户端分支，在服务器分支中添加了对第二次等待 UWorld 时的关闭检查
// 保持 DllMain 最简
// 资源释放逻辑统一


#include "framework.h"               // defines WIN32_LEAN_AND_MEAN and includes <windows.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <cstring>

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
#include "Utility/UserNameFix.h"

using namespace SDK;
// ======================================================
//  SECTION 3 — GLOBAL VARIABLES (now owned by Main)
// ======================================================

// Base address of the host module.  Initialized once at startup.
uintptr_t BaseAddress = 0x0;

// Global pointer used by external modules to access LibReplicate.  The
// underlying object is owned by g_LibReplicateOwner and will be reset
// when the main thread terminates.
LibReplicate* libReplicate = nullptr;

// Raw pointer provided for legacy modules; actual instance is owned by
// g_CmdFrameworkOwner.  Do not delete directly.
static CommandFramework* g_CmdFramework = nullptr;

// RAII owners for dynamically created objects.  They will be reset at
// the end of the main worker thread to ensure proper cleanup.
static std::unique_ptr<LibReplicate> g_LibReplicateOwner;
static std::unique_ptr<CommandFramework> g_CmdFrameworkOwner;
static std::unique_ptr<LateJoinManager> g_LateJoinManagerOwner;

// Shutdown flag used to signal all background loops to exit.  It is
// toggled in DllMain when the DLL is unloaded.
static std::atomic_bool g_ShutdownRequested{ false };

// Handle for the main worker thread.  Stored to optionally wait on
// termination during unload if desired.
static HANDLE g_MainThreadHandle = nullptr;

void OnJoinFromPipe(const std::string& ip, const std::string& token)
{
    // Reject join requests that do not include a token or IP.  This
    // simple check prevents accidental triggers when other processes write
    // unexpected data into the pipe.  A more robust implementation
    // would validate the token against an ACL or secret.
    if (ip.empty())
    {
        ClientDebugLog("[PIPE] Rejected join request: empty IP");
        return;
    }
    if (token.empty())
    {
        ClientDebugLog("[PIPE] Rejected join request: empty token");
        return;
    }

    ClientDebugLog("[PIPE] Join request received: " + ip);
    {
        std::lock_guard<std::mutex> lock(MatchIPMutex);
        MatchIP = ip;
    }

    // Ideally we would defer this call onto the game thread to avoid
    // manipulating Unreal objects from a pipe worker thread.  To
    // maintain compatibility with existing code paths we continue
    // calling into ConnectToMatch()/AutoConnectToMatchFromCmdline().
    if (SDK::UWorld::GetWorld() && SDK::UWorld::GetWorld()->OwningGameInstance)
    {
        ConnectToMatch();
    }
    else
    {
        AutoConnectToMatchFromCmdline();
    }
}

// ======================================================
//  SECTION 15.5 — HELPER FUNCTIONS
// ======================================================

// Parse the process command line and determine if a particular
// argument (case-sensitive) is present.  Uses CommandLineToArgvW to
// avoid false positives when an argument appears as a substring of
// another parameter.  Returns true if the argument is found.
static bool IsArgumentPresent(const wchar_t* wanted)
{
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv)
        return false;

    bool found = false;
    for (int i = 0; i < argc; ++i)
    {
        if (wcscmp(argv[i], wanted) == 0)
        {
            found = true;
            break;
        }
    }

    ::LocalFree(argv);
    return found;
}

// Safely patch the server mode flags at contiguous offsets.  This
// function checks that the offsets are adjacent and uses VirtualProtect
// to temporarily change memory protections.  Returns true on success.
static bool PatchServerModeFlags()
{
    // Resolve the absolute addresses of the two bytes.
    uintptr_t addr0 = BaseAddress + GameOffsets::Memory::ServerModeFlag0;
    uintptr_t addr1 = BaseAddress + GameOffsets::Memory::ServerModeFlag1;
    // Ensure the offsets are adjacent as expected.
    if (addr1 != addr0 + 1)
        return false;

    // Temporarily make the page writable.
    DWORD oldProtect = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(addr0), 2, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    const unsigned char patch[2] = { 0x00, 0x01 };
    std::memcpy(reinterpret_cast<void*>(addr0), patch, sizeof(patch));

    DWORD dummy = 0;
    ::VirtualProtect(reinterpret_cast<void*>(addr0), 2, oldProtect, &dummy);
    return true;
}

// Entry point for the worker thread.  Wraps MainThread in a try/catch to
// prevent unhandled exceptions from bubbling into the loader.  The
// signature matches the Windows API for CreateThread.
static DWORD WINAPI MainThreadThunk(LPVOID)
{
    try
    {
        MainThread();
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("[PAYLOAD] Unhandled exception: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
    }
    catch (...)
    {
        OutputDebugStringA("[PAYLOAD] Unhandled unknown exception\n");
    }
    return 0;
}

// ======================================================
//  SECTION 15 — MAIN THREAD (ENTRY LOGIC)
// ======================================================

void MainThread()
{
    // Record that the payload has started.
    ClientDebugLog("[BOOT] DLL injected, starting...");
    // A flag to indicate whether initialization aborted early.  This allows
    // cleanup code at the end of the function to run even if an early
    // shutdown is requested.  Returning from inside the try block would
    // bypass RAII cleanup, so we avoid direct returns.
    bool exitEarly = false;
    try
    {
        // Suppress the UE4 missing font popup.
        InitMessageBoxHook();

        // Capture the base address of the host module.
        BaseAddress = reinterpret_cast<uintptr_t>(::GetModuleHandleA(nullptr));

        // Initialize Unreal FMemory using the resolved offset.
        UC::FMemory::Init(GameOffsets::Resolve(BaseAddress, GameOffsets::Memory::FMemoryInit));

        // Determine if we are running as a dedicated server via explicit argument.
        amServer = IsArgumentPresent(L"-server");

        // Wait for the world to be ready.  During this time, repeatedly
        // patch the server mode flags if in server mode.  Exit early if
        // the shutdown flag is set.
        while (!SDK::UWorld::GetWorld())
        {
            if (g_ShutdownRequested.load(std::memory_order_acquire))
            {
                exitEarly = true;
                break;
            }
            if (amServer)
            {
                PatchServerModeFlags();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // If we were asked to shut down before the world became ready, skip
        // further initialization.  Do not return directly; instead set
        // exitEarly so that cleanup code will still run.
        if (!exitEarly)
        {
            // Branch into server or client logic.
            if (amServer)
            {
                // Install server-specific hooks.
                InitServerHooks();
                ServerLog("[SERVER] Hooks installed.");

                // Ensure the world is fully ready before proceeding.  In some builds
                // the first call to GetWorld may return a non-null pointer early
                // during loading; wait a bit longer to be safe.
                while (!SDK::UWorld::GetWorld())
                {
                    if (g_ShutdownRequested.load(std::memory_order_acquire))
                    {
                        exitEarly = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!exitEarly)
                {
                    ServerLog("[SERVER] UWorld is ready.");

                    // Initialize LibReplicate.  Use a unique_ptr to manage the lifetime
                    // and update the global libReplicate pointer for compatibility with
                    // existing modules.
                    g_LibReplicateOwner = std::make_unique<LibReplicate>(
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
                    libReplicate = g_LibReplicateOwner.get();
                    ServerLog("[SERVER] LibReplicate initialized.");

                    // Initialize LateJoinManager and assign to the legacy global pointer for
                    // external modules.  Ownership is held by g_LateJoinManagerOwner.
                    g_LateJoinManagerOwner = std::make_unique<LateJoinManager>(
                        DidProcStartMatch,
                        PlayerRespawnAllowedMap,
                        nullptr);
                    gLateJoinManager = g_LateJoinManagerOwner.get();
                    ServerLog("[SERVER] LateJoinManager initialized.");

                    // Kick off server replication and heartbeat.
                    StartServer();
                    StartHeartbeatThread();
                }
            }
            else
            {
                // ==== CLIENT INITIALIZATION ====
                LoadClientConfig();

                // Initialize client debug logging.
                if (ClientDebugLogEnabled)
                {
                    std::filesystem::create_directory("clientlogs");
                    std::string path = "clientlogs/clientlog-" + CurrentTimestamp() + ".txt";
                    clientLogFile.open(path, std::ios::app);
                    std::cout << "[CLIENT] Debug logging enabled: " << path << std::endl;
                }

                // Bring up the debug console and enable the in-game console.
                InitDebugConsole();
                EnableUnrealConsole();

                // Install client-specific hooks.
                InitClientHook();

                // If the -debug flag is present, start the hotkey thread for
                // developer tools.  Use std::thread; detach because there is no
                // explicit join point.  The thread should monitor the shutdown flag.
                if (IsArgumentPresent(L"-debug"))
                {
                    std::thread(HotkeyThread).detach();
                }

                // Initialize custom weapon/ability definitions.
                InitClientArmory();

                // Initialize the command pipe framework if configured.
                if (!MatchPipeName.empty())
                {
                    g_CmdFrameworkOwner = std::make_unique<CommandFramework>();
                    g_CmdFramework = g_CmdFrameworkOwner.get();
                    g_CmdFramework->SetPipeName(MatchPipeName);
                    g_CmdFramework->SetJoinCallback(OnJoinFromPipe);
                    g_CmdFramework->SetLogCallback([](const std::string& msg) { ClientDebugLog(msg); });
                    g_CmdFramework->Start();
                }

                // Auto-connect to any match IP set by command-line overrides.
                bool hasInitialMatchTarget = false;
                {
                    std::lock_guard<std::mutex> lock(MatchIPMutex);
                    hasInitialMatchTarget = !MatchIP.empty();
                }
                if (hasInitialMatchTarget)
                {
                    AutoConnectToMatchFromCmdline();
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        // Log known exceptions via ServerLog.  Do not block waiting for user input.
        ServerLog(std::string("[ERROR] Exception in MainThread: ") + e.what());
    }
    catch (...)
    {
        ServerLog("[ERROR] Unknown exception in MainThread");
    }

    // Clean up resources on thread exit.  Do not wait indefinitely; if shutdown
    // happens early these objects may not have been created.  Always reset
    // pointers, even if initialization aborted early.
    if (g_CmdFrameworkOwner)
    {
        g_CmdFrameworkOwner->Stop();
        g_CmdFrameworkOwner.reset();
        g_CmdFramework = nullptr;
    }
    g_LateJoinManagerOwner.reset();
    g_LibReplicateOwner.reset();
    libReplicate = nullptr;
}

// ======================================================
//  SECTION 16 — DLL ENTRY POINT
// ======================================================

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Prevent the system from creating additional threads for thread attach
        // notifications; we only care about process attach/detach.  This also
        // avoids executing our entry points under the loader lock for other
        // threads.  See MSDN documentation for DisableThreadLibraryCalls【359285536817385†L80-L120】.
        ::DisableThreadLibraryCalls(hModule);

        // Launch the worker thread using the Windows API.  Do not use
        // std::thread here because C++ runtime initialization within
        // DllMain is discouraged and may cause deadlocks【359285536817385†L80-L120】.
        g_MainThreadHandle = ::CreateThread(nullptr, 0, MainThreadThunk, nullptr, 0, nullptr);
        if (g_MainThreadHandle)
        {
            // We do not wait on this handle here.  It will be automatically
            // closed by the system when the process terminates.  If you wish
            // to wait on the thread externally, store the handle as needed.
        }
        break;
    case DLL_PROCESS_DETACH:
        // Signal the worker thread to shut down.  Do not wait on the
        // thread handle here as the loader lock may be held and waiting
        // could cause a deadlock【359285536817385†L80-L120】.  The worker thread
        // will check this flag and exit cleanly.
        g_ShutdownRequested.store(true, std::memory_order_release);
        break;
    default:
        break;
    }
    return TRUE;
}
