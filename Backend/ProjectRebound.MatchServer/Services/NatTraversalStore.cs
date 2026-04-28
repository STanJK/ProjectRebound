using System.Collections.Concurrent;
using System.Net;
using ProjectRebound.Contracts;

namespace ProjectRebound.MatchServer.Services;

public sealed class NatTraversalStore
{
    private readonly ConcurrentDictionary<string, NatBinding> bindings = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<Guid, PunchTicket> punchTickets = new();

    public NatBinding CreateBinding(Guid playerId, int localPort, string? role, Guid? roomId, DateTimeOffset expiresAt)
    {
        var binding = new NatBinding(
            TokenService.NewToken(24),
            playerId,
            localPort,
            Normalize(role, 24),
            roomId,
            null,
            null,
            expiresAt);
        bindings[binding.BindingToken] = binding;
        return binding;
    }

    public NatBinding? ObserveBinding(string token, IPEndPoint remoteEndPoint)
    {
        if (!bindings.TryGetValue(token, out var binding) || binding.ExpiresAt <= DateTimeOffset.UtcNow)
        {
            return null;
        }

        var observed = binding with
        {
            PublicIp = remoteEndPoint.Address.ToString(),
            PublicPort = remoteEndPoint.Port
        };
        bindings[token] = observed;
        return observed;
    }

    public NatBinding? GetBinding(string token, Guid? playerId = null)
    {
        if (!bindings.TryGetValue(token, out var binding) || binding.ExpiresAt <= DateTimeOffset.UtcNow)
        {
            bindings.TryRemove(token, out _);
            return null;
        }

        if (playerId is not null && binding.PlayerId != playerId)
        {
            return null;
        }

        return binding.PublicIp is null || binding.PublicPort is null ? null : binding;
    }

    public PunchTicket CreatePunchTicket(
        Guid roomId,
        Guid hostPlayerId,
        Guid clientPlayerId,
        string hostEndpoint,
        string? hostLocalEndpoint,
        string clientEndpoint,
        string? clientLocalEndpoint,
        TimeSpan ttl)
    {
        var ticket = new PunchTicket(
            Guid.NewGuid(),
            roomId,
            hostPlayerId,
            clientPlayerId,
            hostEndpoint,
            hostLocalEndpoint,
            clientEndpoint,
            clientLocalEndpoint,
            TokenService.NewToken(18),
            PunchTicketState.Pending,
            DateTimeOffset.UtcNow.Add(ttl));
        punchTickets[ticket.TicketId] = ticket;
        return ticket;
    }

    public IReadOnlyList<PunchTicket> GetHostTickets(Guid roomId, Guid hostPlayerId)
    {
        var now = DateTimeOffset.UtcNow;
        var result = new List<PunchTicket>();
        foreach (var ticket in punchTickets.Values)
        {
            if (ticket.ExpiresAt <= now)
            {
                punchTickets.TryUpdate(ticket.TicketId, ticket with { State = PunchTicketState.Expired }, ticket);
                continue;
            }

            if (ticket.RoomId == roomId && ticket.HostPlayerId == hostPlayerId &&
                ticket.State is PunchTicketState.Pending or PunchTicketState.Active)
            {
                var active = ticket.State == PunchTicketState.Pending ? ticket with { State = PunchTicketState.Active } : ticket;
                punchTickets[active.TicketId] = active;
                result.Add(active);
            }
        }

        return result;
    }

    public void Complete(Guid ticketId)
    {
        if (punchTickets.TryGetValue(ticketId, out var ticket))
        {
            punchTickets[ticketId] = ticket with { State = PunchTicketState.Completed };
        }
    }

    public void CleanupExpired()
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var pair in bindings)
        {
            if (pair.Value.ExpiresAt <= now)
            {
                bindings.TryRemove(pair.Key, out _);
            }
        }

        foreach (var pair in punchTickets)
        {
            if (pair.Value.ExpiresAt <= now)
            {
                punchTickets.TryRemove(pair.Key, out _);
            }
        }
    }

    private static string? Normalize(string? value, int maxLength)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }

        var text = value.Trim();
        return text.Length <= maxLength ? text : text[..maxLength];
    }
}

public sealed record NatBinding(
    string BindingToken,
    Guid PlayerId,
    int LocalPort,
    string? Role,
    Guid? RoomId,
    string? PublicIp,
    int? PublicPort,
    DateTimeOffset ExpiresAt);

public sealed record PunchTicket(
    Guid TicketId,
    Guid RoomId,
    Guid HostPlayerId,
    Guid ClientPlayerId,
    string HostEndpoint,
    string? HostLocalEndpoint,
    string ClientEndpoint,
    string? ClientLocalEndpoint,
    string Nonce,
    PunchTicketState State,
    DateTimeOffset ExpiresAt);
