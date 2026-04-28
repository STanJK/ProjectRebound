using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Options;
using ProjectRebound.Contracts;
using ProjectRebound.MatchServer.Data;

namespace ProjectRebound.MatchServer.Services;

public sealed class MatchmakingService(
    IServiceScopeFactory scopeFactory,
    IOptions<MatchServerOptions> options,
    ILogger<MatchmakingService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(2));
        while (!stoppingToken.IsCancellationRequested && await timer.WaitForNextTickAsync(stoppingToken))
        {
            try
            {
                await MatchOnceAsync(stoppingToken);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                logger.LogError(ex, "Matchmaking tick failed.");
            }
        }
    }

    private async Task MatchOnceAsync(CancellationToken cancellationToken)
    {
        await using var scope = scopeFactory.CreateAsyncScope();
        var db = scope.ServiceProvider.GetRequiredService<MatchServerDbContext>();
        var cfg = options.Value;
        var now = DateTimeOffset.UtcNow;

        var waitingTickets = await db.MatchTickets
            .Where(x => x.State == MatchTicketState.Waiting && x.ExpiresAt > now)
            .OrderBy(x => x.CreatedAt)
            .ToListAsync(cancellationToken);

        foreach (var ticket in waitingTickets.ToList())
        {
            var room = await FindOpenRoomAsync(db, ticket, cancellationToken);
            if (room is null)
            {
                continue;
            }

            var activeCount = await RoomOperations.ActiveReservationCountAsync(db, room.RoomId);
            if (Math.Max(room.PlayerCount, activeCount) >= room.MaxPlayers)
            {
                continue;
            }

            var (_, joinTicket) = await RoomOperations.ReserveJoinAsync(db, room, ticket.PlayerId, TimeSpan.FromSeconds(cfg.JoinTicketSeconds));
            ticket.State = MatchTicketState.Matched;
            ticket.AssignedRoomId = room.RoomId;
            ticket.JoinTicketPlain = joinTicket;
            ticket.FailureReason = null;
        }

        await db.SaveChangesAsync(cancellationToken);

        var remaining = await db.MatchTickets
            .Where(x => x.State == MatchTicketState.Waiting && x.ExpiresAt > now && x.CanHost && x.ProbeId != null)
            .OrderBy(x => x.Region)
            .ThenBy(x => x.CreatedAt)
            .ToListAsync(cancellationToken);

        foreach (var ticket in remaining)
        {
            var probe = await db.HostProbes.FirstOrDefaultAsync(x =>
                x.ProbeId == ticket.ProbeId &&
                x.PlayerId == ticket.PlayerId &&
                x.Status == HostProbeStatus.Succeeded &&
                x.ExpiresAt > now, cancellationToken);

            if (probe is null)
            {
                continue;
            }

            var hostToken = TokenService.NewToken(32);
            var room = RoomOperations.CreateRoomFromProbe(
                ticket.PlayerId,
                probe,
                hostToken,
                ticket.RoomName ?? "Quick Match",
                ticket.Region,
                ticket.Map ?? "Warehouse",
                ticket.Mode ?? "pve",
                ticket.Version,
                ticket.MaxPlayers);

            room.State = RoomState.Starting;
            db.Rooms.Add(room);

            ticket.State = MatchTicketState.HostAssigned;
            ticket.AssignedRoomId = room.RoomId;
            ticket.HostTokenPlain = hostToken;
            ticket.FailureReason = null;
            await db.SaveChangesAsync(cancellationToken);
            break;
        }
    }

    private static async Task<Room?> FindOpenRoomAsync(MatchServerDbContext db, MatchTicket ticket, CancellationToken cancellationToken)
    {
        var query = db.Rooms
            .Where(x => (x.State == RoomState.Open || x.State == RoomState.Starting) &&
                x.Region == ticket.Region &&
                x.Version == ticket.Version &&
                x.HostPlayerId != ticket.PlayerId);

        if (!string.IsNullOrWhiteSpace(ticket.Map))
        {
            query = query.Where(x => x.Map == ticket.Map);
        }

        if (!string.IsNullOrWhiteSpace(ticket.Mode))
        {
            query = query.Where(x => x.Mode == ticket.Mode);
        }

        return await query.OrderByDescending(x => x.PlayerCount).ThenBy(x => x.CreatedAt).FirstOrDefaultAsync(cancellationToken);
    }
}
