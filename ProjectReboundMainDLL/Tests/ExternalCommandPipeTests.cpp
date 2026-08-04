#include "../API/ExternalCommandPipe.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    int failures = 0;

    void Expect(const bool condition, const char* const description)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "FAILED: " << description << '\n';
    }

    class TestHandle
    {
    public:
        explicit TestHandle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
            : handle_(handle)
        {
        }

        ~TestHandle()
        {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
                CloseHandle(handle_);
        }

        TestHandle(const TestHandle&) = delete;
        TestHandle& operator=(const TestHandle&) = delete;

        [[nodiscard]] HANDLE Get() const noexcept
        {
            return handle_;
        }

    private:
        HANDLE handle_;
    };

    std::string UniquePipeName(const std::string_view suffix)
    {
        return "ProjectRebound_ExternalCommandPipeTests_" +
            std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(GetTickCount64()) + "_" + std::string(suffix);
    }

    std::wstring PipePath(const std::string& pipeName)
    {
        return L"\\\\.\\pipe\\" + std::wstring(pipeName.begin(), pipeName.end());
    }

    TestHandle ConnectClient(const std::string& pipeName)
    {
        const std::wstring path = PipePath(pipeName);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

        do
        {
            const HANDLE handle = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (handle != INVALID_HANDLE_VALUE)
                return TestHandle(handle);

            Sleep(10);
        } while (std::chrono::steady_clock::now() < deadline);

        return TestHandle();
    }

    bool WriteFrame(const HANDLE pipe, const std::string_view frame)
    {
        DWORD bytesWritten = 0;
        return WriteFile(
            pipe,
            frame.data(),
            static_cast<DWORD>(frame.size()),
            &bytesWritten,
            nullptr) != FALSE && bytesWritten == frame.size();
    }

    std::string ReadFrame(const HANDLE pipe)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        DWORD availableBytes = 0;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &availableBytes, nullptr))
                return {};
            if (availableBytes != 0)
                break;
            Sleep(10);
        }

        if (availableBytes == 0)
            return {};

        std::vector<char> buffer(availableBytes);
        DWORD bytesRead = 0;
        if (!ReadFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            nullptr))
        {
            return {};
        }
        return std::string(buffer.data(), bytesRead);
    }

    void TestInvalidConfiguration()
    {
        ExternalCommandPipe pipeServer;
        pipeServer.SetPipeName("invalid\\name");
        Expect(!pipeServer.Start(), "unsafe pipe name is rejected");
        Expect(!pipeServer.IsRunning(), "rejected pipeServer remains stopped");
    }

    void TestProtocolAndRestart()
    {
        ExternalCommandPipe pipeServer;
        const std::string pipeName = UniquePipeName("protocol");
        std::atomic<unsigned int> joinCalls{0};
        std::atomic<bool> joinRanOnListener{false};

        pipeServer.SetPipeName(pipeName);
        pipeServer.SetWatchdogTimeout(3000);
        pipeServer.SetWriteTimeout(1000);
        pipeServer.SetJoinCallback([&](const std::string&, const std::string&)
            {
                joinRanOnListener.store(pipeServer.IsListenerThread());
                return ++joinCalls == 1;
            });
        pipeServer.SetServerStatusCallback([]()
            {
                return nlohmann::json{
                    {"state", "RUNNING"},
                    {"player_count", 2},
                    {"round_state", "InProgress"}
                };
            });

        Expect(pipeServer.Start(), "pipeServer starts");
        Expect(pipeServer.IsRunning(), "pipeServer reports running");

        TestHandle client = ConnectClient(pipeName);
        Expect(client.Get() != INVALID_HANDLE_VALUE, "same-user client connects");
        if (client.Get() != INVALID_HANDLE_VALUE)
        {
            Expect(WriteFrame(client.Get(), "ping\t{\"request_id\":\"ping-1\"}\n"),
                "ping request is written");
            Expect(ReadFrame(client.Get()) ==
                "pong\t{\"request_id\":\"ping-1\"}\n",
                "pong echoes request id");

            Expect(WriteFrame(client.Get(),
                "join\t{\"ip\":\"127.0.0.1:7777\",\"request_id\":\"join-1\"}\n"),
                "join request is written");
            Expect(ReadFrame(client.Get()).find("join_ack\t") == 0,
                "accepted join receives join_ack");
            Expect(joinRanOnListener.load(), "join callback runs on listener thread");

            Expect(WriteFrame(client.Get(),
                "join\t{\"ip\":\"127.0.0.1:7777\",\"request_id\":\"join-2\"}\n"),
                "second join request is written");
            const std::string busy = ReadFrame(client.Get());
            Expect(busy.find("error\t") == 0 &&
                busy.find("\"code\":\"busy\"") != std::string::npos &&
                busy.find("\"request_id\":\"join-2\"") != std::string::npos,
                "rejected join receives correlated busy error");

            Expect(WriteFrame(client.Get(), "join\t{\"ip\":\"127.0.0.1:0\"}\n"),
                "invalid join request is written");
            Expect(ReadFrame(client.Get()).find("\"code\":\"invalid_target\"") !=
                std::string::npos,
                "invalid target is rejected");

            Expect(WriteFrame(client.Get(),
                "server_status\t{\"request_id\":\"status-1\"}\n"),
                "server status request is written");
            const std::string status = ReadFrame(client.Get());
            Expect(status.find("server_status_ack\t") == 0 &&
                status.find("\"player_count\":2") != std::string::npos &&
                status.find("\"request_id\":\"status-1\"") != std::string::npos,
                "server status response is correlated");
        }

        pipeServer.Stop();
        Expect(!pipeServer.IsRunning(), "pipeServer stops");

        Expect(pipeServer.Start(), "pipeServer restarts after a complete stop");
        pipeServer.Stop();
    }

    void TestStopWhileWaitingForClient()
    {
        ExternalCommandPipe pipeServer;
        pipeServer.SetPipeName(UniquePipeName("stop"));
        Expect(pipeServer.Start(), "waiting pipeServer starts");

        const auto startedAt = std::chrono::steady_clock::now();
        pipeServer.Stop();
        const auto elapsed = std::chrono::steady_clock::now() - startedAt;
        Expect(elapsed < std::chrono::seconds(2),
            "stop cancels pending ConnectNamedPipe promptly");
    }
}

int main()
{
    TestInvalidConfiguration();
    TestProtocolAndRestart();
    TestStopWhileWaitingForClient();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "ExternalCommandPipe tests passed\n";
    return 0;
}
