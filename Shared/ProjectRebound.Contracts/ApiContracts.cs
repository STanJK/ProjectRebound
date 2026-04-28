namespace ProjectRebound.Contracts;

public sealed record ApiError(string Code, string Message, object? Details = null);

public sealed record GuestAuthRequest(string? DisplayName, string? DeviceToken);
public sealed record GuestAuthResponse(Guid PlayerId, string DisplayName, string AccessToken);

public sealed record CreateHostProbeRequest(int Port);
public sealed record CreateHostProbeResponse(Guid ProbeId, string PublicIp, int Port, string Nonce, DateTimeOffset ExpiresAt);
public sealed record ConfirmHostProbeRequest(string Nonce);
public sealed record HostProbeResponse(Guid ProbeId, HostProbeStatus Status, string PublicIp, int Port, DateTimeOffset ExpiresAt);

public sealed record CreateRoomRequest(
    Guid? ProbeId,
    string Name,
    string Region,
    string Map,
    string Mode,
    string Version,
    int MaxPlayers,
    string? BindingToken = null);

public sealed record CreateRoomResponse(Guid RoomId, string HostToken, int HeartbeatSeconds);

public sealed record RoomSummary(
    Guid RoomId,
    Guid? HostPlayerId,
    string Name,
    string Region,
    string Map,
    string Mode,
    string Version,
    string Endpoint,
    int Port,
    int PlayerCount,
    int MaxPlayers,
    string? ServerState,
    RoomState State,
    DateTimeOffset LastSeenAt,
    string? EndedReason);

public sealed record PagedRoomsResponse(IReadOnlyList<RoomSummary> Items, int Page, int PageSize, int Total);

public sealed record JoinRoomRequest(string? Version);
public sealed record JoinRoomResponse(string Connect, string JoinTicket, DateTimeOffset ExpiresAt);

public sealed record LeaveRoomRequest(string? JoinTicket);

public sealed record RoomHeartbeatRequest(string HostToken, int PlayerCount, string? ServerState);
public sealed record RoomLifecycleRequest(string HostToken);
public sealed record RoomHeartbeatResponse(bool Ok, int NextHeartbeatSeconds);

public sealed record CreateMatchTicketRequest(
    string Region,
    string? Map,
    string? Mode,
    string Version,
    bool CanHost,
    Guid? ProbeId,
    string? RoomName,
    int MaxPlayers);

public sealed record CreateMatchTicketResponse(Guid TicketId, MatchTicketState State, DateTimeOffset ExpiresAt);

public sealed record MatchTicketResponse(
    Guid TicketId,
    MatchTicketState State,
    Guid? AssignedRoomId,
    RoomSummary? Room,
    string? HostToken,
    string? Connect,
    string? JoinTicket,
    string? FailureReason,
    DateTimeOffset ExpiresAt);

public sealed record LegacyServerStatusRequest(
    string Name,
    string Region,
    string Mode,
    string Map,
    int Port,
    int PlayerCount,
    string ServerState,
    Guid? RoomId,
    string? HostToken,
    string? Version);

public sealed record LegacyServerStatusResponse(bool Ok, Guid? ServerId, int NextHeartbeatSeconds);

public sealed record CreateNatBindingRequest(int LocalPort, string? Role, Guid? RoomId);
public sealed record CreateNatBindingResponse(string BindingToken, string UdpHost, int UdpPort, DateTimeOffset ExpiresAt);
public sealed record ConfirmNatBindingResponse(
    string BindingToken,
    string PublicIp,
    int PublicPort,
    int LocalPort,
    string? Role,
    Guid? RoomId,
    DateTimeOffset ExpiresAt);

public sealed record CreatePunchTicketRequest(string JoinTicket, string BindingToken, string? ClientLocalEndpoint);
public sealed record CreatePunchTicketResponse(
    Guid TicketId,
    PunchTicketState State,
    string Nonce,
    string HostEndpoint,
    string? HostLocalEndpoint,
    string ClientEndpoint,
    string? ClientLocalEndpoint,
    DateTimeOffset ExpiresAt);

public sealed record PunchTicketResponse(
    Guid TicketId,
    PunchTicketState State,
    string Nonce,
    string HostEndpoint,
    string? HostLocalEndpoint,
    string ClientEndpoint,
    string? ClientLocalEndpoint,
    DateTimeOffset ExpiresAt);

public sealed record ListPunchTicketsResponse(IReadOnlyList<PunchTicketResponse> Items);

public sealed record CreateRelayAllocationRequest(Guid RoomId, string Role, string? HostToken, string? JoinTicket);
public sealed record CreateRelayAllocationResponse(
    Guid SessionId,
    string Role,
    string Secret,
    string RelayHost,
    int RelayPort,
    DateTimeOffset ExpiresAt);
