using Microsoft.EntityFrameworkCore;
using ProjectRebound.Contracts;
using ProjectRebound.MatchServer.Data;

namespace ProjectRebound.MatchServer.Services;

public static class AuthExtensions
{
    public static async Task<Player?> GetBearerPlayerAsync(this HttpContext httpContext, MatchServerDbContext db)
    {
        var header = httpContext.Request.Headers.Authorization.ToString();
        const string prefix = "Bearer ";
        if (string.IsNullOrWhiteSpace(header) || !header.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        var token = header[prefix.Length..].Trim();
        if (token.Length == 0)
        {
            return null;
        }

        var tokenHash = TokenService.Hash(token);
        var player = await db.Players.FirstOrDefaultAsync(x => x.DeviceTokenHash == tokenHash);
        if (player is null || player.Status != PlayerStatus.Active)
        {
            return null;
        }

        player.LastSeenAt = DateTimeOffset.UtcNow;
        return player;
    }

    public static IResult UnauthorizedError()
    {
        return Results.Json(new ApiError("UNAUTHORIZED", "Bearer player token is required."), statusCode: StatusCodes.Status401Unauthorized);
    }
}
