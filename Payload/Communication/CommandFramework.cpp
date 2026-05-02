// ======================================================
//  CommandFramework — 运行时指令框架实现
// ======================================================
//
//  架构概述：
//    单 ListenerLoop 线程 + Overlapped I/O 完成所有管道操作。
//    无独立看门狗线程 — 读超时由 WaitForSingleObject(overlappedEvent, 1s)
//    循环轮询实现，同时每 1s 检查 running 标志以支持快速 Stop()。
//
//  线程模型：
//    - ListenerLoop 线程：负责 ConnectNamedPipe + ReadFile + 解析分发
//    - 回调（onJoin / onLog）在 ListenerLoop 线程内同步执行
//    - SendResponse 可从任意线程调用，受 writeMutex 保护
//
//  管道生命周期：
//    CreateNamedPipe → ConnectNamedPipe(overlapped) → 读循环 →
//    断线 / 超时 → DisconnectNamedPipe + CloseHandle → 重新 Create

#include "CommandFramework.h"

// =====================================================================
//  构造 / 析构
// =====================================================================

CommandFramework::CommandFramework()
    : watchdogTimeoutMs(30000)
    , running(false)
    , hCurrentPipe(INVALID_HANDLE_VALUE)
    , saInitialized(false)
{
    ZeroMemory(&sa, sizeof(sa));
    ZeroMemory(&sd, sizeof(sd));
}

CommandFramework::~CommandFramework()
{
    Stop();
}

// =====================================================================
//  配置 — 必须在 Start() 前调用
// =====================================================================

void CommandFramework::SetPipeName(const std::string& name)
{
    pipeName = R"(\\.\pipe\)" + name;
}

void CommandFramework::SetWatchdogTimeout(DWORD timeoutMs)
{
    watchdogTimeoutMs = timeoutMs;
}

void CommandFramework::SetJoinCallback(JoinCallback cb)
{
    onJoin = std::move(cb);
}

void CommandFramework::SetLogCallback(LogCallback cb)
{
    onLog = std::move(cb);
}

void CommandFramework::SetDebugCallback(DebugCallback cb)
{
    onDebug = std::move(cb);
}

// =====================================================================
//  生命周期
// =====================================================================

bool CommandFramework::Start()
{
    if (running.load())
        return false;
    if (pipeName.empty())
        return false;

    // 构建 NULL DACL 安全描述符，允许任意进程连接管道
    if (!saInitialized)
    {
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
        saInitialized = true;
    }

    running.store(true);
    listenerThread = std::thread(&CommandFramework::ListenerLoop, this);
    Log("[CMDFW] Started on pipe: " + pipeName);
    return true;
}

void CommandFramework::Stop()
{
    if (!running.load())
        return;

    running.store(false);

    // 唤醒 ListenerLoop 若其正阻塞在 I/O 上
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        if (hCurrentPipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(hCurrentPipe, nullptr);
        }
    }

    if (listenerThread.joinable())
    {
        auto nativeHandle = listenerThread.native_handle();
        DWORD waitResult = WaitForSingleObject(nativeHandle, 5000);
        if (waitResult == WAIT_TIMEOUT)
        {
            // 线程未能及时退出（可能卡在内核调用），detach 交由系统回收
            Log("[CMDFW] Listener thread did not exit in time, detaching.");
            listenerThread.detach();
        }
        else
        {
            listenerThread.join();
        }
    }

    Log("[CMDFW] Stopped.");
}

// =====================================================================
//  SendResponse — 线程安全的管道写入
// =====================================================================

void CommandFramework::SendResponse(const std::string& cmd, const nlohmann::json& args)
{
    std::lock_guard<std::mutex> lock(writeMutex);
    if (hCurrentPipe == INVALID_HANDLE_VALUE)
        return;

    std::string line = cmd + PROTOCOL_DELIM + args.dump() + PROTOCOL_NEWLINE;
    DWORD written = 0;
    WriteFile(hCurrentPipe, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}

// =====================================================================
//  ListenerLoop — 主监听循环（单线程 + Overlapped I/O）
// =====================================================================

void CommandFramework::ListenerLoop()
{
    while (running.load())
    {
        // ── 创建管道实例 ──
        HANDLE hPipe = CreateNamedPipeA(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,                         // 单实例（当前仅支持一个启动器连接）
            4096,                      // 输出缓冲区
            4096,                      // 输入缓冲区
            0,                         // 默认超时
            &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            Log("[CMDFW] CreateNamedPipe failed: " + std::to_string(GetLastError()));
            if (!running.load()) break;
            Sleep(1000);
            continue;
        }

        // ── 等待客户端连接（overlapped，每 1s 检查 running 标志）──
        OVERLAPPED connectOl = {};
        connectOl.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(hPipe, &connectOl);
        DWORD connectErr = GetLastError();

        if (!connected && connectErr == ERROR_IO_PENDING)
        {
            bool clientConnected = false;
            while (running.load())
            {
                DWORD waitResult = WaitForSingleObject(connectOl.hEvent, 1000);
                if (waitResult == WAIT_OBJECT_0)
                {
                    clientConnected = true;
                    break;
                }
                if (waitResult == WAIT_FAILED)
                    break;
                // WAIT_TIMEOUT → 继续循环检查 running 标志
            }

            if (!clientConnected)
            {
                CancelIo(hPipe);
                CloseHandle(connectOl.hEvent);
                CloseHandle(hPipe);
                continue;
            }
        }
        else if (!connected && connectErr != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(connectOl.hEvent);
            CloseHandle(hPipe);
            if (running.load()) Sleep(1000);
            continue;
        }

        CloseHandle(connectOl.hEvent);

        // ── 客户端已连接，开始读循环 ──
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            hCurrentPipe = hPipe;
        }

        Log("[CMDFW] Client connected.");

        char buf[4096];
        std::string lineBuf;

        while (running.load())
        {
            OVERLAPPED readOl = {};
            readOl.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

            DWORD bytesRead = 0;
            BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, &readOl);

            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                bool dataReady = false;
                DWORD waitedMs = 0;
                while (running.load())
                {
                    DWORD waitSliceMs = 1000;
                    if (watchdogTimeoutMs > 0)
                    {
                        DWORD remainingMs = watchdogTimeoutMs > waitedMs ? watchdogTimeoutMs - waitedMs : 0;
                        if (remainingMs == 0)
                            break;
                        if (remainingMs < waitSliceMs)
                            waitSliceMs = remainingMs;
                    }

                    DWORD waitResult = WaitForSingleObject(readOl.hEvent, waitSliceMs);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        dataReady = true;
                        break;
                    }
                    if (waitResult == WAIT_FAILED)
                        break;
                    if (waitResult == WAIT_TIMEOUT && watchdogTimeoutMs > 0)
                    {
                        waitedMs += waitSliceMs;
                        if (waitedMs >= watchdogTimeoutMs)
                            break;
                    }
                }

                if (!dataReady)
                {
                    // 超时或被 Stop() 的 CancelIo 中断
                    CancelIo(hPipe);
                    CloseHandle(readOl.hEvent);
                    break;
                }

                GetOverlappedResult(hPipe, &readOl, &bytesRead, FALSE);
            }

            CloseHandle(readOl.hEvent);

            if (bytesRead == 0)
            {
                // 客户端正常断开
                break;
            }

            buf[bytesRead] = '\0';
            lineBuf.append(buf, bytesRead);

            // 按 '\n' 拆分为完整行
            size_t pos;
            while ((pos = lineBuf.find(PROTOCOL_NEWLINE)) != std::string::npos)
            {
                std::string line = lineBuf.substr(0, pos);
                lineBuf.erase(0, pos + 1);

                // 兼容 Windows 风格 \r\n
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                if (!line.empty())
                    ParseAndDispatch(line);
            }

            // 安全阀：缓冲区异常膨胀则重置
            if (lineBuf.size() > 65536)
            {
                Log("[CMDFW] Line buffer exceeded 64K, resetting.");
                lineBuf.clear();
            }
        }

        Log("[CMDFW] Client disconnected.");

        // 清除连接句柄，阻止新的 SendResponse
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            hCurrentPipe = INVALID_HANDLE_VALUE;
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

// =====================================================================
//  协议解析与分发
// =====================================================================

// @brief 按 '\t' 拆分为 CMD 和 JSON 字符串，解析 JSON 后分发。
//        解析失败则回写 error 响应。
void CommandFramework::ParseAndDispatch(const std::string& line)
{
    size_t delim = line.find(PROTOCOL_DELIM);
    if (delim == std::string::npos)
    {
        nlohmann::json resp;
        resp["msg"] = "missing delimiter";
        SendResponse("error", resp);
        return;
    }

    std::string cmd = line.substr(0, delim);
    std::string jsonStr = line.substr(delim + 1);

    nlohmann::json args;
    if (!jsonStr.empty())
    {
        try
        {
            args = nlohmann::json::parse(jsonStr);
        }
        catch (const nlohmann::json::parse_error& e)
        {
            nlohmann::json resp;
            resp["msg"] = "json parse error";
            resp["detail"] = e.what();
            SendResponse("error", resp);
            return;
        }
    }

    Dispatch(cmd, args);
}

// @brief 命令分发：ping → pong / join → 触发回调 + join_ack / 其他 → error
void CommandFramework::Dispatch(const std::string& cmd, const nlohmann::json& args)
{
    if (cmd == "ping")
    {
        SendResponse("pong", nlohmann::json::object());
    }
    else if (cmd == "join")
    {
        std::string ip = args.value("ip", "");
        std::string token = args.value("token", "");
        if (!ip.empty() && onJoin)
        {
            onJoin(ip, token);
        }
        nlohmann::json ack;
        ack["status"] = "ok";
        SendResponse("join_ack", ack);
    }
    else if (cmd == "debug")
    {
        if (onDebug)
        {
            nlohmann::json result = onDebug(args);
            SendResponse("debug_ack", result);
        }
        else
        {
            nlohmann::json resp;
            resp["ok"] = false;
            resp["error"] = "no debug handler registered";
            SendResponse("debug_ack", resp);
        }
    }
    else
    {
        Log("[CMDFW] Unknown command: " + cmd);
        nlohmann::json resp;
        resp["msg"] = "unknown command";
        resp["cmd"] = cmd;
        SendResponse("error", resp);
    }
}

// =====================================================================
//  内部工具
// =====================================================================

void CommandFramework::Log(const std::string& msg)
{
    if (onLog)
        onLog(msg);
}
