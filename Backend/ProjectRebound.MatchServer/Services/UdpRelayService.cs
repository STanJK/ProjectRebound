using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Options;

namespace ProjectRebound.MatchServer.Services;

public sealed class UdpRelayService(
    RelayStore store,
    IOptions<MatchServerOptions> options,
    ILogger<UdpRelayService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var port = options.Value.UdpRelayPort;
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, port));
        logger.LogInformation("UDP relay service listening on 0.0.0.0:{Port}", port);

        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                var result = await udp.ReceiveAsync(stoppingToken);
                if (TryHandleControlPacket(udp, result.Buffer, result.RemoteEndPoint))
                {
                    continue;
                }

                foreach (var target in store.GetForwardTargets(result.RemoteEndPoint))
                {
                    await udp.SendAsync(result.Buffer, result.Buffer.Length, target);
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "UDP relay packet failed.");
            }
        }
    }

    private bool TryHandleControlPacket(UdpClient udp, byte[] buffer, IPEndPoint remoteEndPoint)
    {
        if (buffer.Length == 0 || buffer[0] != (byte)'{')
        {
            return false;
        }

        RelayRegisterPacket? packet;
        try
        {
            packet = JsonSerializer.Deserialize<RelayRegisterPacket>(Encoding.UTF8.GetString(buffer), JsonOptions);
        }
        catch (JsonException)
        {
            return false;
        }

        if (packet?.Type != "PRB_RELAY_REGISTER_V1" ||
            packet.SessionId is null ||
            string.IsNullOrWhiteSpace(packet.Role) ||
            string.IsNullOrWhiteSpace(packet.Secret))
        {
            return false;
        }

        var registration = store.ObserveRegistration(packet.SessionId.Value, packet.Role, packet.Secret, remoteEndPoint);
        logger.LogInformation(
            "UDP relay registration {Status}: session={SessionId} role={Role} remote={RemoteEndPoint}",
            registration is null ? "rejected" : "accepted",
            packet.SessionId,
            packet.Role,
            remoteEndPoint);

        var response = registration is null
            ? JsonSerializer.Serialize(new
            {
                type = "PRB_RELAY_REGISTERED_V1",
                ok = false,
                message = "relay registration rejected"
            }, JsonOptions)
            : JsonSerializer.Serialize(new
            {
                type = "PRB_RELAY_REGISTERED_V1",
                ok = true,
                sessionId = registration.SessionId,
                role = registration.Role,
                observedIp = registration.EndPoint.Address.ToString(),
                observedPort = registration.EndPoint.Port
            }, JsonOptions);

        var bytes = Encoding.UTF8.GetBytes(response);
        udp.Send(bytes, bytes.Length, remoteEndPoint);
        return true;
    }

    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private sealed record RelayRegisterPacket(string? Type, Guid? SessionId, string? Role, string? Secret);
}
