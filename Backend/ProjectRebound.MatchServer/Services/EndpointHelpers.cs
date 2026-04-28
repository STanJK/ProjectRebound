using System.Net;
using Microsoft.EntityFrameworkCore;
using ProjectRebound.Contracts;
using ProjectRebound.MatchServer.Data;

namespace ProjectRebound.MatchServer.Services;

public static class EndpointHelpers
{
    public static string GetPublicIp(HttpContext context)
    {
        var forwardedFor = context.Request.Headers["X-Forwarded-For"].FirstOrDefault();
        if (!string.IsNullOrWhiteSpace(forwardedFor))
        {
            return forwardedFor.Split(',')[0].Trim();
        }

        var remote = context.Connection.RemoteIpAddress;
        if (remote is null)
        {
            return "127.0.0.1";
        }

        if (remote.IsIPv4MappedToIPv6)
        {
            remote = remote.MapToIPv4();
        }

        return remote.Equals(IPAddress.IPv6Loopback) ? "127.0.0.1" : remote.ToString();
    }

    public static RoomSummary ToSummary(Room room)
    {
        return new RoomSummary(
            room.RoomId,
            room.HostPlayerId,
            room.Name,
            room.Region,
            room.Map,
            room.Mode,
            room.Version,
            room.Endpoint,
            room.Port,
            room.PlayerCount,
            room.MaxPlayers,
            room.ServerState,
            room.State,
            room.LastSeenAt,
            room.EndedReason);
    }

    public static async Task<IResult> NotFoundRoomAsync(Guid roomId, MatchServerDbContext db)
    {
        var exists = await db.Rooms.AnyAsync(x => x.RoomId == roomId);
        return exists
            ? Results.Json(new ApiError("ROOM_NOT_AVAILABLE", "Room is not available for this operation."), statusCode: 409)
            : Results.Json(new ApiError("NOT_FOUND", "Room was not found."), statusCode: 404);
    }
}
