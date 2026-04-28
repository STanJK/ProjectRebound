using Microsoft.EntityFrameworkCore;
using ProjectRebound.Contracts;
using ProjectRebound.MatchServer.Data;

namespace ProjectRebound.MatchServer.Services;

public static class RoomOperations
{
    public static bool IsJoinable(Room room)
    {
        return room.State is RoomState.Open or RoomState.Starting;
    }

    public static async Task<int> ActiveReservationCountAsync(MatchServerDbContext db, Guid roomId)
    {
        var now = DateTimeOffset.UtcNow;
        return await db.RoomPlayers.CountAsync(x =>
            x.RoomId == roomId &&
            (x.Status == RoomPlayerStatus.Reserved || x.Status == RoomPlayerStatus.Joined) &&
            x.ExpiresAt > now);
    }

    public static async Task<(RoomPlayer Reservation, string JoinTicket)> ReserveJoinAsync(
        MatchServerDbContext db,
        Room room,
        Guid playerId,
        TimeSpan ttl)
    {
        var now = DateTimeOffset.UtcNow;
        var existing = await db.RoomPlayers.FirstOrDefaultAsync(x =>
            x.RoomId == room.RoomId &&
            x.PlayerId == playerId &&
            (x.Status == RoomPlayerStatus.Reserved || x.Status == RoomPlayerStatus.Joined));

        var joinTicket = TokenService.NewToken(24);
        if (existing is not null)
        {
            existing.JoinTicketHash = TokenService.Hash(joinTicket);
            existing.Status = RoomPlayerStatus.Reserved;
            existing.JoinedAt = now;
            existing.ExpiresAt = now.Add(ttl);
            return (existing, joinTicket);
        }

        var reservation = new RoomPlayer
        {
            RoomPlayerId = Guid.NewGuid(),
            RoomId = room.RoomId,
            PlayerId = playerId,
            JoinTicketHash = TokenService.Hash(joinTicket),
            Status = RoomPlayerStatus.Reserved,
            JoinedAt = now,
            ExpiresAt = now.Add(ttl)
        };

        db.RoomPlayers.Add(reservation);
        return (reservation, joinTicket);
    }

    public static Room CreateRoomFromProbe(
        Guid hostPlayerId,
        HostProbe probe,
        string hostToken,
        string name,
        string region,
        string map,
        string mode,
        string version,
        int maxPlayers)
    {
        var now = DateTimeOffset.UtcNow;
        var endpoint = $"{probe.PublicIp}:{probe.Port}";
        return new Room
        {
            RoomId = Guid.NewGuid(),
            HostPlayerId = hostPlayerId,
            HostProbeId = probe.ProbeId,
            HostTokenHash = TokenService.Hash(hostToken),
            Name = string.IsNullOrWhiteSpace(name) ? "ProjectRebound Room" : name.Trim(),
            Region = string.IsNullOrWhiteSpace(region) ? "CN" : region.Trim(),
            Map = string.IsNullOrWhiteSpace(map) ? "Warehouse" : map.Trim(),
            Mode = string.IsNullOrWhiteSpace(mode) ? "pve" : mode.Trim(),
            Version = string.IsNullOrWhiteSpace(version) ? "dev" : version.Trim(),
            Endpoint = endpoint,
            Port = probe.Port,
            MaxPlayers = Math.Clamp(maxPlayers, 1, 128),
            PlayerCount = 1,
            ServerState = "Forming",
            State = RoomState.Open,
            CreatedAt = now,
            LastSeenAt = now
        };
    }

    public static Room CreateRoomFromEndpoint(
        Guid hostPlayerId,
        string endpoint,
        int port,
        string hostToken,
        string name,
        string region,
        string map,
        string mode,
        string version,
        int maxPlayers)
    {
        var now = DateTimeOffset.UtcNow;
        return new Room
        {
            RoomId = Guid.NewGuid(),
            HostPlayerId = hostPlayerId,
            HostTokenHash = TokenService.Hash(hostToken),
            Name = string.IsNullOrWhiteSpace(name) ? "ProjectRebound Room" : name.Trim(),
            Region = string.IsNullOrWhiteSpace(region) ? "CN" : region.Trim(),
            Map = string.IsNullOrWhiteSpace(map) ? "Warehouse" : map.Trim(),
            Mode = string.IsNullOrWhiteSpace(mode) ? "pve" : mode.Trim(),
            Version = string.IsNullOrWhiteSpace(version) ? "dev" : version.Trim(),
            Endpoint = endpoint,
            Port = port,
            MaxPlayers = Math.Clamp(maxPlayers, 1, 128),
            PlayerCount = 1,
            ServerState = "Forming",
            State = RoomState.Open,
            CreatedAt = now,
            LastSeenAt = now
        };
    }
}
