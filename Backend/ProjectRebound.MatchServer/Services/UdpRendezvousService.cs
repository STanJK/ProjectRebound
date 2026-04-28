using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Options;

namespace ProjectRebound.MatchServer.Services;

public sealed class UdpRendezvousService(
    NatTraversalStore store,
    IOptions<MatchServerOptions> options,
    ILogger<UdpRendezvousService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var port = options.Value.UdpRendezvousPort;
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Any, port));
        logger.LogInformation("UDP rendezvous service listening on 0.0.0.0:{Port}", port);

        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                var result = await udp.ReceiveAsync(stoppingToken);
                var message = Encoding.UTF8.GetString(result.Buffer);
                var request = JsonSerializer.Deserialize<RendezvousPacket>(message, JsonOptions);
                if (string.IsNullOrWhiteSpace(request?.Token))
                {
                    continue;
                }

                var binding = store.ObserveBinding(request.Token, result.RemoteEndPoint);
                if (binding is null)
                {
                    continue;
                }

                var response = JsonSerializer.Serialize(new
                {
                    type = "nat-binding",
                    token = binding.BindingToken,
                    publicIp = binding.PublicIp,
                    publicPort = binding.PublicPort
                }, JsonOptions);
                var bytes = Encoding.UTF8.GetBytes(response);
                await udp.SendAsync(bytes, bytes.Length, result.RemoteEndPoint);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception ex)
            {
                logger.LogWarning(ex, "UDP rendezvous packet failed.");
            }
        }
    }

    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private sealed record RendezvousPacket(string? Type, string? Token, int? LocalPort);
}
