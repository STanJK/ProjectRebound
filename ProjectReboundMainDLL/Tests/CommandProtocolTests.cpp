#include "../API/CommandProtocol.h"

#include <iostream>
#include <stdexcept>
#include <string>

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

    void TestValidFrames()
    {
        const auto ping = CommandProtocol::ParseFrame("ping\t{}");
        Expect(ping.Succeeded(), "ping frame parses");
        Expect(ping.request && ping.request->command == "ping", "ping command preserved");

        const auto join = CommandProtocol::ParseFrame(
            "join\t{\"ip\":\"127.0.0.1:7777\",\"request_id\":\"req-1\"}");
        Expect(join.Succeeded(), "join frame parses");
        Expect(join.requestId == std::optional<std::string>("req-1"), "request id extracted");
        Expect(join.request && join.request->arguments["ip"] == "127.0.0.1:7777", "join IP preserved");

        const auto status = CommandProtocol::ParseFrame(
            "server_status\t{\"request_id\":\"status-1\"}");
        Expect(status.Succeeded(), "server_status frame parses");
        Expect(status.request && status.request->command == "server_status",
            "server_status command preserved");
    }

    void TestInvalidFrames()
    {
        Expect(!CommandProtocol::ParseFrame("").Succeeded(), "empty frame rejected");
        Expect(!CommandProtocol::ParseFrame("ping{}").Succeeded(), "missing delimiter rejected");
        Expect(!CommandProtocol::ParseFrame("ping\t").Succeeded(), "empty JSON rejected");
        Expect(!CommandProtocol::ParseFrame("ping\t[]").Succeeded(), "array JSON rejected");
        Expect(!CommandProtocol::ParseFrame("ping\tnull").Succeeded(), "null JSON rejected");
        Expect(!CommandProtocol::ParseFrame("ping\t{broken}").Succeeded(), "malformed JSON rejected");
        Expect(!CommandProtocol::ParseFrame("Ping\t{}").Succeeded(), "uppercase command rejected");
        Expect(!CommandProtocol::ParseFrame("ping\t{\"request_id\":42}").Succeeded(), "non-string request id rejected");

        std::string oversized = "debug\t{\"value\":\"";
        oversized.append(CommandProtocol::MaxFrameBytes, 'x');
        oversized.append("\"}");
        Expect(!CommandProtocol::ParseFrame(oversized).Succeeded(), "oversized frame rejected");
    }

    void TestFrameSizeBoundary()
    {
        const std::string prefix = "debug\t{\"value\":\"";
        const std::string suffix = "\"}";
        std::string maximumFrame = prefix;
        maximumFrame.append(
            CommandProtocol::MaxFrameBytes - 1U - prefix.size() - suffix.size(),
            'x');
        maximumFrame.append(suffix);
        Expect(maximumFrame.size() + 1U == CommandProtocol::MaxFrameBytes,
            "maximum request wire frame is exactly 64 KiB");
        Expect(CommandProtocol::ParseFrame(maximumFrame).Succeeded(),
            "maximum request wire frame is accepted");

        maximumFrame.insert(maximumFrame.size() - suffix.size(), 1, 'x');
        Expect(!CommandProtocol::ParseFrame(maximumFrame).Succeeded(),
            "request exceeding wire limit by one byte is rejected");

        constexpr std::size_t encodedFrameOverhead =
            sizeof("debug\t{\"value\":\"\"}\n") - 1U;
        nlohmann::json maximumPayload{
            {"value", std::string(
                CommandProtocol::MaxFrameBytes - encodedFrameOverhead,
                'x')}
        };
        Expect(CommandProtocol::EncodeFrame("debug", maximumPayload).size() ==
            CommandProtocol::MaxFrameBytes,
            "maximum response wire frame is accepted");

        maximumPayload["value"] = maximumPayload["value"].get<std::string>() + "x";
        bool threw = false;
        try
        {
            (void)CommandProtocol::EncodeFrame("debug", maximumPayload);
        }
        catch (const std::length_error&)
        {
            threw = true;
        }
        Expect(threw, "response exceeding wire limit by one byte is rejected");
    }

    void TestMatchTargets()
    {
        Expect(CommandProtocol::ValidateMatchTarget("127.0.0.1:7777"), "IPv4 target accepted");
        Expect(CommandProtocol::ValidateMatchTarget("game.example.test:443"), "hostname target accepted");
        Expect(CommandProtocol::ValidateMatchTarget("[2001:db8::1]:7777"), "bracketed IPv6 target accepted");
        Expect(!CommandProtocol::ValidateMatchTarget("127.0.0.1"), "missing port rejected");
        Expect(!CommandProtocol::ValidateMatchTarget("127.0.0.1:0"), "zero port rejected");
        Expect(!CommandProtocol::ValidateMatchTarget("127.0.0.1:65536"), "oversized port rejected");
        Expect(!CommandProtocol::ValidateMatchTarget("127.0.0.1:7777;quit"), "console injection rejected");
        Expect(!CommandProtocol::ValidateMatchTarget("2001:db8::1:7777"), "unbracketed IPv6 rejected");
    }

    void TestResponses()
    {
        const auto payload = CommandProtocol::WithRequestId(
            nlohmann::json{{"status", "accepted"}},
            std::optional<std::string>("req-2"));
        Expect(payload["request_id"] == "req-2", "response request id attached");

        const std::string frame = CommandProtocol::EncodeFrame("join_ack", payload);
        Expect(frame == "join_ack\t{\"request_id\":\"req-2\",\"status\":\"accepted\"}\n",
            "response frame encoded deterministically");

        const auto error = CommandProtocol::MakeError("invalid_request", "bad request");
        Expect(error["code"] == "invalid_request", "error code encoded");

        const std::string statusFrame = CommandProtocol::EncodeFrame(
            "server_status_ack",
            nlohmann::json{{"player_count", 2}, {"state", "RUNNING"}});
        Expect(statusFrame ==
            "server_status_ack\t{\"player_count\":2,\"state\":\"RUNNING\"}\n",
            "server status response is encoded deterministically");
    }
}

int main()
{
    TestValidFrames();
    TestInvalidFrames();
    TestFrameSizeBoundary();
    TestMatchTargets();
    TestResponses();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "CommandProtocol tests passed\n";
    return 0;
}
