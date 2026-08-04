#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CommandProtocol.h"

class ExternalCommandPipe
{
public:
    using JoinCallback = std::function<bool(const std::string& ip, const std::string& token)>;
    using LogCallback = std::function<void(const std::string& message)>;
    using DebugCallback = std::function<nlohmann::json(const nlohmann::json& arguments)>;
    using ServerStatusCallback = std::function<nlohmann::json()>;

    ExternalCommandPipe();
    ~ExternalCommandPipe();

    ExternalCommandPipe(const ExternalCommandPipe&) = delete;
    ExternalCommandPipe& operator=(const ExternalCommandPipe&) = delete;
    ExternalCommandPipe(ExternalCommandPipe&&) = delete;
    ExternalCommandPipe& operator=(ExternalCommandPipe&&) = delete;

    // Configuration becomes immutable while the listener is running.
    void SetPipeName(const std::string& name);
    void SetWatchdogTimeout(DWORD timeoutMs);
    void SetWriteTimeout(DWORD timeoutMs);
    void SetJoinCallback(JoinCallback callback);
    void SetLogCallback(LogCallback callback);
    void SetDebugCallback(DebugCallback callback);
    void SetServerStatusCallback(ServerStatusCallback callback);

    [[nodiscard]] bool Start();
    void Stop() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] bool IsListenerThread() const noexcept;
    [[nodiscard]] bool SendResponse(const std::string& command, const nlohmann::json& payload);

private:
    class UniqueHandle
    {
    public:
        UniqueHandle() noexcept = default;
        explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
        ~UniqueHandle() { Reset(); }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
                Reset(other.Release());
            return *this;
        }

        [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
        [[nodiscard]] bool IsValid() const noexcept
        {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }

        HANDLE Release() noexcept
        {
            const HANDLE handle = handle_;
            handle_ = INVALID_HANDLE_VALUE;
            return handle;
        }

        void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        {
            if (IsValid())
                CloseHandle(handle_);
            handle_ = handle;
        }

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };

    enum class IoStatus
    {
        Completed,
        TimedOut,
        Stopped,
        Failed
    };

    struct IoResult
    {
        IoStatus status = IoStatus::Failed;
        DWORD bytesTransferred = 0;
        DWORD error = ERROR_SUCCESS;
    };

    enum class FrameResult
    {
        Processed,
        ProtocolError,
        TransportError
    };

    [[nodiscard]] bool BuildPipePath(std::string& failureReason);
    [[nodiscard]] bool InitializeSecurity(std::string& failureReason);
    void ReleaseSecurity() noexcept;
    [[nodiscard]] bool IsAuthorizedClient(HANDLE pipe) const;

    void ListenerLoop() noexcept;
    [[nodiscard]] bool ReadClient(HANDLE pipe);

    [[nodiscard]] IoResult CompleteIo(HANDLE handle, OVERLAPPED& operation) const noexcept;
    [[nodiscard]] IoResult WaitForPendingIo(
        HANDLE handle,
        OVERLAPPED& operation,
        DWORD timeoutMs) const noexcept;
    void CancelAndDrain(HANDLE handle, OVERLAPPED& operation) const noexcept;

    [[nodiscard]] FrameResult ParseAndDispatch(const std::string& frame) noexcept;
    [[nodiscard]] FrameResult Dispatch(const CommandProtocol::Request& request) noexcept;
    [[nodiscard]] bool SendError(
        std::string_view code,
        std::string_view message,
        const std::optional<std::string>& requestId = std::nullopt);

    void Log(const std::string& message) const noexcept;
    void LogWin32Error(const std::string& operation, DWORD error) const noexcept;

    std::string pipeName;
    std::wstring pipePath;
    DWORD watchdogTimeoutMs = 30000;
    DWORD writeTimeoutMs = 5000;

    std::atomic<bool> running{false};
    std::atomic<bool> connectionFaulted{false};
    std::atomic<DWORD> listenerThreadId{0};
    HANDLE hCurrentPipe = INVALID_HANDLE_VALUE; // Non-owning; guarded by writeMutex.
    UniqueHandle stopEvent;
    bool stopping = false; // Guarded by lifecycleMutex.

    mutable std::mutex lifecycleMutex;
    mutable std::mutex callbackMutex;
    mutable std::mutex writeMutex;
    std::thread listenerThread;

    JoinCallback onJoin;
    LogCallback onLog;
    DebugCallback onDebug;
    ServerStatusCallback onServerStatus;

    SECURITY_ATTRIBUTES securityAttributes{};
    SECURITY_DESCRIPTOR securityDescriptor{};
    PACL pipeAcl = nullptr;
    std::vector<unsigned char> allowedUserSid;
    bool securityInitialized = false;
};
