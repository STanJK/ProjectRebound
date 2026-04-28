using System.Net;
using System.Net.Sockets;

namespace ProjectRebound.MatchServer.Services;

public sealed class UdpProbeSender(ILogger<UdpProbeSender> logger)
{
    public async Task SendAsync(string host, int port, string nonce, CancellationToken cancellationToken)
    {
        using var udp = new UdpClient(AddressFamily.InterNetwork);
        var bytes = System.Text.Encoding.UTF8.GetBytes(nonce);
        try
        {
            await udp.SendAsync(bytes, bytes.Length, host, port);
        }
        catch (Exception ex) when (ex is SocketException or ObjectDisposedException)
        {
            logger.LogWarning(ex, "Failed to send UDP probe to {Host}:{Port}", host, port);
        }
    }
}
