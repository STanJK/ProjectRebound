#include "CommandProtocol.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace
{
    bool IsCommandCharacter(const unsigned char ch) noexcept
    {
        return (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-';
    }

    bool IsHostCharacter(const unsigned char ch) noexcept
    {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '-' || ch == '_';
    }

    bool IsIpv6Character(const unsigned char ch) noexcept
    {
        return std::isxdigit(ch) != 0 || ch == ':' || ch == '.' ||
            ch == '%' || ch == '-' || ch == '_';
    }

    bool ParsePort(const std::string_view portText) noexcept
    {
        if (portText.empty() || portText.size() > 5)
            return false;

        unsigned int port = 0;
        for (const unsigned char ch : portText)
        {
            if (ch < '0' || ch > '9')
                return false;

            port = port * 10U + static_cast<unsigned int>(ch - '0');
            if (port > 65535U)
                return false;
        }

        return port != 0;
    }

    CommandProtocol::ParseResult Failure(
        std::string code,
        std::string message,
        std::optional<std::string> requestId = std::nullopt)
    {
        CommandProtocol::ParseResult result;
        result.errorCode = std::move(code);
        result.errorMessage = std::move(message);
        result.requestId = std::move(requestId);
        return result;
    }
}

namespace CommandProtocol
{
    ParseResult ParseFrame(const std::string_view frame)
    {
        if (frame.empty())
            return Failure("empty_frame", "frame is empty");
        // ParseFrame receives the frame without its trailing LF. Keep the
        // wire-size limit symmetric with EncodeFrame, which includes that LF.
        if (frame.size() >= MaxFrameBytes)
            return Failure("frame_too_large", "frame exceeds 64 KiB");

        const std::size_t delimiter = frame.find(Delimiter);
        if (delimiter == std::string_view::npos)
            return Failure("missing_delimiter", "command and JSON must be separated by a tab");
        if (delimiter == 0 || delimiter > MaxCommandBytes)
            return Failure("invalid_command", "command length is invalid");

        const std::string_view command = frame.substr(0, delimiter);
        if (!std::all_of(command.begin(), command.end(), [](const unsigned char ch)
            {
                return IsCommandCharacter(ch);
            }))
        {
            return Failure("invalid_command", "command contains unsupported characters");
        }

        const std::string_view jsonText = frame.substr(delimiter + 1);
        if (jsonText.empty())
            return Failure("missing_json", "JSON object is required");

        try
        {
            nlohmann::json arguments = nlohmann::json::parse(jsonText.begin(), jsonText.end());
            if (!arguments.is_object())
                return Failure("invalid_json_type", "JSON payload must be an object");

            std::optional<std::string> requestId;
            if (const auto id = arguments.find("request_id"); id != arguments.end())
            {
                if (!id->is_string())
                    return Failure("invalid_request_id", "request_id must be a string");

                requestId = id->get<std::string>();
                if (requestId->empty() || requestId->size() > MaxRequestIdBytes)
                    return Failure("invalid_request_id", "request_id length is invalid");
            }

            ParseResult result;
            result.request = Request{
                std::string(command),
                std::move(arguments),
                requestId
            };
            result.requestId = std::move(requestId);
            return result;
        }
        catch (const nlohmann::json::parse_error&)
        {
            return Failure("invalid_json", "JSON payload could not be parsed");
        }
        catch (const nlohmann::json::exception&)
        {
            return Failure("invalid_json", "JSON payload is invalid");
        }
        catch (...)
        {
            return Failure("internal_error", "request parsing failed");
        }
    }

    bool ValidateMatchTarget(
        const std::string_view target,
        std::string* const failureReason)
    {
        if (failureReason != nullptr)
            failureReason->clear();

        const auto fail = [failureReason](const char* const reason)
        {
            if (failureReason != nullptr)
                *failureReason = reason;
            return false;
        };

        if (target.empty())
            return fail("target is empty");
        if (target.size() > MaxMatchTargetBytes)
            return fail("target is too long");

        for (const unsigned char ch : target)
        {
            if (ch < 0x21 || ch > 0x7e)
                return fail("target must contain printable ASCII without whitespace");
        }

        std::string_view host;
        std::string_view port;
        bool isBracketedIpv6 = false;

        if (target.front() == '[')
        {
            const std::size_t closingBracket = target.find(']');
            if (closingBracket == std::string_view::npos ||
                closingBracket <= 1 ||
                closingBracket + 1 >= target.size() ||
                target[closingBracket + 1] != ':')
            {
                return fail("bracketed IPv6 target must use [address]:port");
            }

            host = target.substr(1, closingBracket - 1);
            port = target.substr(closingBracket + 2);
            isBracketedIpv6 = true;
        }
        else
        {
            const std::size_t colon = target.rfind(':');
            if (colon == std::string_view::npos || colon == 0 || colon + 1 >= target.size())
                return fail("target must use host:port");
            if (target.find(':') != colon)
                return fail("IPv6 targets must use [address]:port");

            host = target.substr(0, colon);
            port = target.substr(colon + 1);
        }

        const bool hostValid = std::all_of(host.begin(), host.end(), [isBracketedIpv6](const unsigned char ch)
            {
                return isBracketedIpv6 ? IsIpv6Character(ch) : IsHostCharacter(ch);
            });
        if (!hostValid)
            return fail("host contains unsupported characters");
        if (!ParsePort(port))
            return fail("port must be between 1 and 65535");

        return true;
    }

    nlohmann::json WithRequestId(
        nlohmann::json payload,
        const std::optional<std::string>& requestId)
    {
        if (!payload.is_object())
            payload = nlohmann::json{{"result", std::move(payload)}};
        if (requestId.has_value())
            payload["request_id"] = *requestId;
        return payload;
    }

    nlohmann::json MakeError(
        const std::string_view code,
        const std::string_view message,
        const std::optional<std::string>& requestId)
    {
        return WithRequestId(
            nlohmann::json{
                {"code", std::string(code)},
                {"message", std::string(message)}
            },
            requestId);
    }

    std::string EncodeFrame(
        const std::string_view command,
        const nlohmann::json& payload)
    {
        if (command.empty() || command.size() > MaxCommandBytes ||
            !std::all_of(command.begin(), command.end(), [](const unsigned char ch)
                {
                    return IsCommandCharacter(ch);
                }))
        {
            throw std::invalid_argument("invalid response command");
        }
        if (!payload.is_object())
            throw std::invalid_argument("response payload must be a JSON object");

        const std::string serializedPayload = payload.dump();
        std::string frame;
        frame.reserve(command.size() + serializedPayload.size() + 2U);
        frame.append(command);
        frame.push_back(Delimiter);
        frame.append(serializedPayload);
        frame.push_back(Newline);

        if (frame.size() > MaxFrameBytes)
            throw std::length_error("response frame exceeds 64 KiB");
        return frame;
    }
}
