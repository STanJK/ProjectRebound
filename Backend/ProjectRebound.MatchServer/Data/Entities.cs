using ProjectRebound.Contracts;

namespace ProjectRebound.MatchServer.Data;

public sealed class Player
{
    public Guid PlayerId { get; set; }
    public string DisplayName { get; set; } = "";
    public string DeviceTokenHash { get; set; } = "";
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset LastSeenAt { get; set; }
    public PlayerStatus Status { get; set; } = PlayerStatus.Active;
}

public sealed class HostProbe
{
    public Guid ProbeId { get; set; }
    public Guid PlayerId { get; set; }
    public string PublicIp { get; set; } = "";
    public int Port { get; set; }
    public string Nonce { get; set; } = "";
    public HostProbeStatus Status { get; set; } = HostProbeStatus.Pending;
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset ExpiresAt { get; set; }
}

public sealed class Room
{
    public Guid RoomId { get; set; }
    public Guid? HostPlayerId { get; set; }
    public Guid? HostProbeId { get; set; }
    public string HostTokenHash { get; set; } = "";
    public string Name { get; set; } = "";
    public string Region { get; set; } = "";
    public string Map { get; set; } = "";
    public string Mode { get; set; } = "";
    public string Version { get; set; } = "";
    public string Endpoint { get; set; } = "";
    public int Port { get; set; }
    public int MaxPlayers { get; set; }
    public int PlayerCount { get; set; }
    public string? ServerState { get; set; }
    public RoomState State { get; set; } = RoomState.Open;
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset LastSeenAt { get; set; }
    public string? EndedReason { get; set; }
}

public sealed class RoomPlayer
{
    public Guid RoomPlayerId { get; set; }
    public Guid RoomId { get; set; }
    public Guid PlayerId { get; set; }
    public string JoinTicketHash { get; set; } = "";
    public RoomPlayerStatus Status { get; set; } = RoomPlayerStatus.Reserved;
    public DateTimeOffset JoinedAt { get; set; }
    public DateTimeOffset ExpiresAt { get; set; }
}

public sealed class MatchTicket
{
    public Guid TicketId { get; set; }
    public Guid PlayerId { get; set; }
    public string Region { get; set; } = "";
    public string? Map { get; set; }
    public string? Mode { get; set; }
    public string Version { get; set; } = "";
    public bool CanHost { get; set; }
    public Guid? ProbeId { get; set; }
    public string? RoomName { get; set; }
    public int MaxPlayers { get; set; }
    public MatchTicketState State { get; set; } = MatchTicketState.Waiting;
    public Guid? AssignedRoomId { get; set; }
    public string? HostTokenPlain { get; set; }
    public string? JoinTicketPlain { get; set; }
    public string? FailureReason { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset ExpiresAt { get; set; }
}

public sealed class LegacyServer
{
    public Guid ServerId { get; set; }
    public string SourceKey { get; set; } = "";
    public string Name { get; set; } = "";
    public string Region { get; set; } = "";
    public string Mode { get; set; } = "";
    public string Map { get; set; } = "";
    public string Endpoint { get; set; } = "";
    public int Port { get; set; }
    public int PlayerCount { get; set; }
    public string ServerState { get; set; } = "";
    public string? Version { get; set; }
    public DateTimeOffset LastSeenAt { get; set; }
}
