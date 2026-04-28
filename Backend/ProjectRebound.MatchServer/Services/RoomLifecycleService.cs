using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Options;
using ProjectRebound.Contracts;
using ProjectRebound.MatchServer.Data;

namespace ProjectRebound.MatchServer.Services;

public sealed class RoomLifecycleService(
    IServiceScopeFactory scopeFactory,
    IOptions<MatchServerOptions> options,
    ILogger<RoomLifecycleService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(5));
        while (!stoppingToken.IsCancellationRequested && await timer.WaitForNextTickAsync(stoppingToken))
        {
            try
            {
                await SweepAsync(stoppingToken);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                logger.LogError(ex, "Room lifecycle sweep failed.");
            }
        }
    }

    private async Task SweepAsync(CancellationToken cancellationToken)
    {
        await using var scope = scopeFactory.CreateAsyncScope();
        var db = scope.ServiceProvider.GetRequiredService<MatchServerDbContext>();
        var cfg = options.Value;
        var now = DateTimeOffset.UtcNow;

        await db.HostProbes
            .Where(x => x.Status == HostProbeStatus.Pending && x.ExpiresAt <= now)
            .ExecuteUpdateAsync(x => x.SetProperty(p => p.Status, HostProbeStatus.Expired), cancellationToken);

        await db.RoomPlayers
            .Where(x => x.Status == RoomPlayerStatus.Reserved && x.ExpiresAt <= now)
            .ExecuteUpdateAsync(x => x.SetProperty(p => p.Status, RoomPlayerStatus.Expired), cancellationToken);

        await db.MatchTickets
            .Where(x => x.State == MatchTicketState.Waiting && x.ExpiresAt <= now)
            .ExecuteUpdateAsync(x => x
                .SetProperty(p => p.State, MatchTicketState.Expired)
                .SetProperty(p => p.FailureReason, "ticket_expired"), cancellationToken);

        var lostCutoff = now.AddSeconds(-cfg.HostLostAfterSeconds);
        var lostRooms = await db.Rooms
            .Where(x => (x.State == RoomState.Open || x.State == RoomState.Starting || x.State == RoomState.InGame) && x.LastSeenAt <= lostCutoff)
            .ToListAsync(cancellationToken);

        foreach (var room in lostRooms)
        {
            room.State = RoomState.Ended;
            room.EndedReason = "host_lost";
            room.LastSeenAt = now;

            await db.MatchTickets
                .Where(x => x.AssignedRoomId == room.RoomId &&
                    (x.State == MatchTicketState.Waiting || x.State == MatchTicketState.HostAssigned || x.State == MatchTicketState.Matched))
                .ExecuteUpdateAsync(x => x
                    .SetProperty(p => p.State, MatchTicketState.Failed)
                    .SetProperty(p => p.FailureReason, "host_lost"), cancellationToken);
        }

        var retentionCutoff = now.AddMinutes(-cfg.EndedRoomRetentionMinutes);
        await db.Rooms
            .Where(x => (x.State == RoomState.Ended || x.State == RoomState.Expired) && x.LastSeenAt <= retentionCutoff)
            .ExecuteDeleteAsync(cancellationToken);

        await db.SaveChangesAsync(cancellationToken);
    }
}
