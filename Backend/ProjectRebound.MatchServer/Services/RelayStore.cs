using System.Collections.Concurrent;
using System.Net;

namespace ProjectRebound.MatchServer.Services;

public sealed class RelayStore
{
    private readonly ConcurrentDictionary<Guid, RelaySession> sessions = new();
    private readonly ConcurrentDictionary<string, RelayRoute> routes = new(StringComparer.Ordinal);

    public RelayAllocation CreateHostAllocation(Guid roomId, Guid hostPlayerId, DateTimeOffset expiresAt)
    {
        var session = sessions.AddOrUpdate(
            roomId,
            _ => RelaySession.Create(roomId, hostPlayerId, expiresAt),
            (_, existing) => existing with
            {
                HostPlayerId = hostPlayerId,
                HostSecret = TokenService.NewToken(24),
                ExpiresAt = Max(existing.ExpiresAt, expiresAt)
            });

        return new RelayAllocation(session.RoomId, "host", session.HostSecret, session.ExpiresAt);
    }

    public RelayAllocation CreateClientAllocation(Guid roomId, Guid hostPlayerId, Guid clientPlayerId, DateTimeOffset expiresAt)
    {
        var session = sessions.AddOrUpdate(
            roomId,
            _ => RelaySession.Create(roomId, hostPlayerId, expiresAt),
            (_, existing) => existing with
            {
                HostPlayerId = hostPlayerId,
                ExpiresAt = Max(existing.ExpiresAt, expiresAt)
            });

        var secret = TokenService.NewToken(24);
        var client = new RelayClient(clientPlayerId, secret, null, expiresAt);
        session.Clients[secret] = client;
        return new RelayAllocation(session.RoomId, "client", secret, expiresAt);
    }

    public RelayRegistration? ObserveRegistration(Guid sessionId, string role, string secret, IPEndPoint remoteEndPoint)
    {
        CleanupExpired();
        if (!sessions.TryGetValue(sessionId, out var session) || session.ExpiresAt <= DateTimeOffset.UtcNow)
        {
            return null;
        }

        var normalizedRole = role.Trim().ToLowerInvariant();
        if (normalizedRole == "host")
        {
            if (!TokenService.FixedTimeEquals(session.HostSecret, secret))
            {
                return null;
            }

            if (session.HostEndPoint is not null)
            {
                routes.TryRemove(EndpointKey(session.HostEndPoint), out _);
            }

            var updated = session with { HostEndPoint = remoteEndPoint };
            sessions[sessionId] = updated;
            routes[EndpointKey(remoteEndPoint)] = new RelayRoute(sessionId, "host", secret);
            return new RelayRegistration(sessionId, "host", remoteEndPoint);
        }

        if (normalizedRole == "client" && session.Clients.TryGetValue(secret, out var client))
        {
            if (client.ExpiresAt <= DateTimeOffset.UtcNow)
            {
                session.Clients.TryRemove(secret, out _);
                return null;
            }

            if (client.EndPoint is not null)
            {
                routes.TryRemove(EndpointKey(client.EndPoint), out _);
            }

            session.Clients[secret] = client with { EndPoint = remoteEndPoint };
            routes[EndpointKey(remoteEndPoint)] = new RelayRoute(sessionId, "client", secret);
            return new RelayRegistration(sessionId, "client", remoteEndPoint);
        }

        return null;
    }

    public IReadOnlyList<IPEndPoint> GetForwardTargets(IPEndPoint source)
    {
        CleanupExpired();
        if (!routes.TryGetValue(EndpointKey(source), out var route) ||
            !sessions.TryGetValue(route.SessionId, out var session))
        {
            return Array.Empty<IPEndPoint>();
        }

        if (route.Role == "host")
        {
            return session.Clients.Values
                .Where(x => x.ExpiresAt > DateTimeOffset.UtcNow && x.EndPoint is not null)
                .Select(x => x.EndPoint!)
                .ToList();
        }

        if (session.HostEndPoint is null)
        {
            return Array.Empty<IPEndPoint>();
        }

        return new[] { session.HostEndPoint };
    }

    public void CleanupExpired()
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var sessionPair in sessions)
        {
            var session = sessionPair.Value;
            if (session.ExpiresAt <= now)
            {
                sessions.TryRemove(sessionPair.Key, out _);
                RemoveSessionRoutes(sessionPair.Key);
                continue;
            }

            foreach (var clientPair in session.Clients)
            {
                if (clientPair.Value.ExpiresAt <= now)
                {
                    session.Clients.TryRemove(clientPair.Key, out _);
                    if (clientPair.Value.EndPoint is not null)
                    {
                        routes.TryRemove(EndpointKey(clientPair.Value.EndPoint), out _);
                    }
                }
            }
        }
    }

    private void RemoveSessionRoutes(Guid sessionId)
    {
        foreach (var route in routes)
        {
            if (route.Value.SessionId == sessionId)
            {
                routes.TryRemove(route.Key, out _);
            }
        }
    }

    private static string EndpointKey(IPEndPoint endpoint)
    {
        return $"{endpoint.Address}:{endpoint.Port}";
    }

    private static DateTimeOffset Max(DateTimeOffset left, DateTimeOffset right)
    {
        return left >= right ? left : right;
    }
}

public sealed record RelayAllocation(Guid SessionId, string Role, string Secret, DateTimeOffset ExpiresAt);

public sealed record RelayRegistration(Guid SessionId, string Role, IPEndPoint EndPoint);

internal sealed record RelayRoute(Guid SessionId, string Role, string Secret);

internal sealed record RelaySession(
    Guid RoomId,
    Guid HostPlayerId,
    string HostSecret,
    IPEndPoint? HostEndPoint,
    ConcurrentDictionary<string, RelayClient> Clients,
    DateTimeOffset ExpiresAt)
{
    public static RelaySession Create(Guid roomId, Guid hostPlayerId, DateTimeOffset expiresAt)
    {
        return new RelaySession(
            roomId,
            hostPlayerId,
            TokenService.NewToken(24),
            null,
            new ConcurrentDictionary<string, RelayClient>(StringComparer.Ordinal),
            expiresAt);
    }
}

internal sealed record RelayClient(Guid PlayerId, string Secret, IPEndPoint? EndPoint, DateTimeOffset ExpiresAt);
