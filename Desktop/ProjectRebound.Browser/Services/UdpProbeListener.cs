using System.Net.Sockets;
using System.Text;

namespace ProjectRebound.Browser.Services;

public sealed class UdpProbeListener
{
    public async Task<string> WaitForNonceAsync(int port, TimeSpan timeout, CancellationToken cancellationToken)
    {
        using var udp = new UdpClient(port);
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(timeout);

        try
        {
            var result = await udp.ReceiveAsync(timeoutCts.Token);
            return Encoding.UTF8.GetString(result.Buffer);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException("UDP probe timed out. Check port forwarding or firewall settings.");
        }
    }
}
