using System.Text.Json.Serialization;
using Microsoft.EntityFrameworkCore;
using ProjectRebound.Contracts;
using ProjectRebound.MatchServer.Data;
using ProjectRebound.MatchServer.Services;

var builder = WebApplication.CreateBuilder(args);

builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.Converters.Add(new JsonStringEnumConverter());
});

builder.Services.Configure<MatchServerOptions>(builder.Configuration.GetSection("MatchServer"));
builder.Services.AddDbContext<MatchServerDbContext>(options =>
    options.UseSqlite(builder.Configuration.GetConnectionString("MatchServer")));
builder.Services.AddSingleton<UdpProbeSender>();
builder.Services.AddSingleton<NatTraversalStore>();
builder.Services.AddSingleton<RelayStore>();
builder.Services.AddHostedService<UdpRendezvousService>();
builder.Services.AddHostedService<UdpRelayService>();
builder.Services.AddHostedService<RoomLifecycleService>();
builder.Services.AddHostedService<MatchmakingService>();
builder.Services.AddEndpointsApiExplorer();

var app = builder.Build();

using (var scope = app.Services.CreateScope())
{
    var db = scope.ServiceProvider.GetRequiredService<MatchServerDbContext>();
    await db.Database.EnsureCreatedAsync();
    await db.Database.ExecuteSqlRawAsync("PRAGMA journal_mode=WAL;");
}

app.MapGet("/health", () => Results.Ok(new
{
    ok = true,
    service = "ProjectRebound.MatchServer",
    build = "udp-relay-fallback-20260421"
}));

app.MapPost("/v1/auth/guest", async (GuestAuthRequest request, MatchServerDbContext db) =>
{
    var now = DateTimeOffset.UtcNow;
    var token = string.IsNullOrWhiteSpace(request.DeviceToken) ? TokenService.NewToken() : request.DeviceToken.Trim();
    var tokenHash = TokenService.Hash(token);
    var displayName = NormalizeText(request.DisplayName, "Guest", 32);

    var player = await db.Players.FirstOrDefaultAsync(x => x.DeviceTokenHash == tokenHash);
    if (player is null)
    {
        player = new Player
        {
            PlayerId = Guid.NewGuid(),
            DisplayName = displayName,
            DeviceTokenHash = tokenHash,
            CreatedAt = now,
            LastSeenAt = now,
            Status = PlayerStatus.Active
        };
        db.Players.Add(player);
    }
    else
    {
        player.DisplayName = displayName;
        player.LastSeenAt = now;
    }

    await db.SaveChangesAsync();
    return Results.Ok(new GuestAuthResponse(player.PlayerId, player.DisplayName, token));
});

app.MapPost("/v1/host-probes", async (
    CreateHostProbeRequest request,
    HttpContext http,
    MatchServerDbContext db,
    UdpProbeSender probeSender,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    if (request.Port is < 1 or > 65535)
    {
        return BadRequest("VALIDATION_ERROR", "port must be between 1 and 65535");
    }

    var now = DateTimeOffset.UtcNow;
    var publicIp = EndpointHelpers.GetPublicIp(http);
    var nonce = TokenService.NewToken(18);
    var probe = new HostProbe
    {
        ProbeId = Guid.NewGuid(),
        PlayerId = player.PlayerId,
        PublicIp = publicIp,
        Port = request.Port,
        Nonce = nonce,
        Status = HostProbeStatus.Pending,
        CreatedAt = now,
        ExpiresAt = now.AddSeconds(options.Value.HostProbeSeconds)
    };

    db.HostProbes.Add(probe);
    await db.SaveChangesAsync(cancellationToken);
    await probeSender.SendAsync(publicIp, request.Port, nonce, cancellationToken);

    return Results.Ok(new CreateHostProbeResponse(probe.ProbeId, publicIp, probe.Port, nonce, probe.ExpiresAt));
});

app.MapPost("/v1/host-probes/{probeId:guid}/confirm", async (
    Guid probeId,
    ConfirmHostProbeRequest request,
    HttpContext http,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var now = DateTimeOffset.UtcNow;
    var probe = await db.HostProbes.FirstOrDefaultAsync(x => x.ProbeId == probeId && x.PlayerId == player.PlayerId, cancellationToken);
    if (probe is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Host probe was not found."));
    }

    if (probe.ExpiresAt <= now)
    {
        probe.Status = HostProbeStatus.Expired;
        await db.SaveChangesAsync(cancellationToken);
        return Results.Json(new ApiError("PROBE_EXPIRED", "Host probe has expired."), statusCode: 409);
    }

    if (!string.Equals(probe.Nonce, request.Nonce, StringComparison.Ordinal))
    {
        probe.Status = HostProbeStatus.Failed;
        await db.SaveChangesAsync(cancellationToken);
        return Results.Json(new ApiError("PROBE_CONFIRM_FAILED", "Host probe nonce did not match."), statusCode: 409);
    }

    probe.Status = HostProbeStatus.Succeeded;
    await db.SaveChangesAsync(cancellationToken);
    return Results.Ok(new HostProbeResponse(probe.ProbeId, probe.Status, probe.PublicIp, probe.Port, probe.ExpiresAt));
});

app.MapPost("/v1/nat/bindings", async (
    CreateNatBindingRequest request,
    HttpContext http,
    NatTraversalStore nat,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    MatchServerDbContext db) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    if (request.LocalPort is < 1 or > 65535)
    {
        return BadRequest("VALIDATION_ERROR", "localPort must be between 1 and 65535");
    }

    var binding = nat.CreateBinding(
        player.PlayerId,
        request.LocalPort,
        request.Role,
        request.RoomId,
        DateTimeOffset.UtcNow.AddSeconds(options.Value.NatBindingSeconds));

    return Results.Ok(new CreateNatBindingResponse(
        binding.BindingToken,
        http.Request.Host.Host,
        options.Value.UdpRendezvousPort,
        binding.ExpiresAt));
});

app.MapPost("/v1/nat/bindings/{bindingToken}/confirm", async (
    string bindingToken,
    HttpContext http,
    NatTraversalStore nat,
    MatchServerDbContext db) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var binding = nat.GetBinding(bindingToken, player.PlayerId);
    if (binding is null)
    {
        return Results.Json(new ApiError("NAT_BINDING_NOT_READY", "No UDP rendezvous packet has been observed for this binding."), statusCode: 409);
    }

    return Results.Ok(new ConfirmNatBindingResponse(
        binding.BindingToken,
        binding.PublicIp!,
        binding.PublicPort!.Value,
        binding.LocalPort,
        binding.Role,
        binding.RoomId,
        binding.ExpiresAt));
});

app.MapPost("/v1/rooms", async (
    CreateRoomRequest request,
    HttpContext http,
    MatchServerDbContext db,
    NatTraversalStore nat,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var now = DateTimeOffset.UtcNow;
    var hostToken = TokenService.NewToken();
    Room room;
    if (!string.IsNullOrWhiteSpace(request.BindingToken))
    {
        var binding = nat.GetBinding(request.BindingToken, player.PlayerId);
        if (binding is null)
        {
            return Results.Json(new ApiError("NAT_BINDING_REQUIRED", "A fresh successful NAT binding is required before creating a proxy room."), statusCode: 409);
        }

        var endpoint = $"{binding.PublicIp}:{binding.PublicPort}";
        room = RoomOperations.CreateRoomFromEndpoint(
            player.PlayerId,
            endpoint,
            binding.PublicPort!.Value,
            hostToken,
            request.Name,
            request.Region,
            request.Map,
            request.Mode,
            request.Version,
            request.MaxPlayers);
    }
    else
    {
        if (request.ProbeId is null)
        {
            return Results.Json(new ApiError("HOST_PROBE_REQUIRED", "A fresh successful host probe is required before creating a room."), statusCode: 409);
        }

        var probe = await db.HostProbes.FirstOrDefaultAsync(x =>
            x.ProbeId == request.ProbeId &&
            x.PlayerId == player.PlayerId &&
            x.Status == HostProbeStatus.Succeeded &&
            x.ExpiresAt > now, cancellationToken);
        if (probe is null)
        {
            return Results.Json(new ApiError("HOST_PROBE_REQUIRED", "A fresh successful host probe is required before creating a room."), statusCode: 409);
        }

        room = RoomOperations.CreateRoomFromProbe(
            player.PlayerId,
            probe,
            hostToken,
            request.Name,
            request.Region,
            request.Map,
            request.Mode,
            request.Version,
            request.MaxPlayers);
    }

    db.Rooms.Add(room);
    await db.SaveChangesAsync(cancellationToken);
    return Results.Ok(new CreateRoomResponse(room.RoomId, hostToken, options.Value.HeartbeatSeconds));
});

app.MapGet("/v1/rooms", async (
    string? region,
    string? map,
    string? mode,
    string? version,
    string? state,
    int? page,
    int? pageSize,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var currentPage = Math.Max(page ?? 1, 1);
    var currentPageSize = Math.Clamp(pageSize ?? 20, 1, 100);
    var query = db.Rooms.AsNoTracking();

    if (!string.IsNullOrWhiteSpace(region))
    {
        query = query.Where(x => x.Region == region);
    }

    if (!string.IsNullOrWhiteSpace(map))
    {
        query = query.Where(x => x.Map == map);
    }

    if (!string.IsNullOrWhiteSpace(mode))
    {
        query = query.Where(x => x.Mode == mode);
    }

    if (!string.IsNullOrWhiteSpace(version))
    {
        query = query.Where(x => x.Version == version);
    }

    if (!string.IsNullOrWhiteSpace(state) && Enum.TryParse<RoomState>(state, true, out var parsedState))
    {
        query = query.Where(x => x.State == parsedState);
    }
    else
    {
        query = query.Where(x => x.State == RoomState.Open || x.State == RoomState.Starting);
    }

    var total = await query.CountAsync(cancellationToken);
    var items = await query
        .OrderByDescending(x => x.PlayerCount)
        .ThenByDescending(x => x.LastSeenAt)
        .Skip((currentPage - 1) * currentPageSize)
        .Take(currentPageSize)
        .Select(x => EndpointHelpers.ToSummary(x))
        .ToListAsync(cancellationToken);

    return Results.Ok(new PagedRoomsResponse(items, currentPage, currentPageSize, total));
});

app.MapGet("/v1/rooms/{roomId:guid}", async (Guid roomId, MatchServerDbContext db, CancellationToken cancellationToken) =>
{
    var room = await db.Rooms.AsNoTracking().FirstOrDefaultAsync(x => x.RoomId == roomId, cancellationToken);
    return room is null ? Results.NotFound(new ApiError("NOT_FOUND", "Room was not found.")) : Results.Ok(EndpointHelpers.ToSummary(room));
});

app.MapPost("/v1/rooms/{roomId:guid}/join", async (
    Guid roomId,
    JoinRoomRequest request,
    HttpContext http,
    MatchServerDbContext db,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var room = await db.Rooms.FirstOrDefaultAsync(x => x.RoomId == roomId, cancellationToken);
    if (room is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Room was not found."));
    }

    if (!RoomOperations.IsJoinable(room))
    {
        return await EndpointHelpers.NotFoundRoomAsync(roomId, db);
    }

    if (!string.IsNullOrWhiteSpace(request.Version) && !string.Equals(room.Version, request.Version, StringComparison.OrdinalIgnoreCase))
    {
        return Results.Json(new ApiError("VERSION_MISMATCH", "Room version does not match the client version."), statusCode: 409);
    }

    var activeCount = await RoomOperations.ActiveReservationCountAsync(db, roomId);
    if (Math.Max(room.PlayerCount, activeCount) >= room.MaxPlayers && room.HostPlayerId != player.PlayerId)
    {
        return Results.Json(new ApiError("ROOM_FULL", "Room is full."), statusCode: 409);
    }

    var (reservation, joinTicket) = await RoomOperations.ReserveJoinAsync(db, room, player.PlayerId, TimeSpan.FromSeconds(options.Value.JoinTicketSeconds));
    await db.SaveChangesAsync(cancellationToken);
    return Results.Ok(new JoinRoomResponse(room.Endpoint, joinTicket, reservation.ExpiresAt));
});

app.MapPost("/v1/rooms/{roomId:guid}/leave", async (
    Guid roomId,
    LeaveRoomRequest request,
    HttpContext http,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var query = db.RoomPlayers.Where(x =>
        x.RoomId == roomId &&
        x.PlayerId == player.PlayerId &&
        (x.Status == RoomPlayerStatus.Reserved || x.Status == RoomPlayerStatus.Joined));

    if (!string.IsNullOrWhiteSpace(request.JoinTicket))
    {
        var ticketHash = TokenService.Hash(request.JoinTicket);
        query = query.Where(x => x.JoinTicketHash == ticketHash);
    }

    var updated = await query.ExecuteUpdateAsync(x => x.SetProperty(p => p.Status, RoomPlayerStatus.Left), cancellationToken);
    return Results.Ok(new { ok = true, updated });
});

app.MapPost("/v1/rooms/{roomId:guid}/punch-tickets", async (
    Guid roomId,
    CreatePunchTicketRequest request,
    HttpContext http,
    MatchServerDbContext db,
    NatTraversalStore nat,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var room = await db.Rooms.AsNoTracking().FirstOrDefaultAsync(x => x.RoomId == roomId, cancellationToken);
    if (room is null || room.HostPlayerId is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Room was not found."));
    }

    if (!RoomOperations.IsJoinable(room))
    {
        return await EndpointHelpers.NotFoundRoomAsync(roomId, db);
    }

    var now = DateTimeOffset.UtcNow;
    var joinTicketHash = TokenService.Hash(request.JoinTicket);
    var reservation = await db.RoomPlayers.AsNoTracking().FirstOrDefaultAsync(x =>
        x.RoomId == roomId &&
        x.PlayerId == player.PlayerId &&
        x.JoinTicketHash == joinTicketHash &&
        (x.Status == RoomPlayerStatus.Reserved || x.Status == RoomPlayerStatus.Joined) &&
        x.ExpiresAt > now, cancellationToken);
    if (reservation is null)
    {
        return Results.Json(new ApiError("JOIN_TICKET_REQUIRED", "A fresh join ticket is required before creating a punch ticket."), statusCode: 409);
    }

    var binding = nat.GetBinding(request.BindingToken, player.PlayerId);
    if (binding is null)
    {
        return Results.Json(new ApiError("NAT_BINDING_REQUIRED", "A fresh successful client NAT binding is required."), statusCode: 409);
    }

    var ticket = nat.CreatePunchTicket(
        room.RoomId,
        room.HostPlayerId.Value,
        player.PlayerId,
        room.Endpoint,
        null,
        $"{binding.PublicIp}:{binding.PublicPort}",
        request.ClientLocalEndpoint,
        TimeSpan.FromSeconds(options.Value.PunchTicketSeconds));

    return Results.Ok(ToPunchTicketResponse(ticket));
});

app.MapGet("/v1/rooms/{roomId:guid}/punch-tickets", async (
    Guid roomId,
    string hostToken,
    MatchServerDbContext db,
    NatTraversalStore nat,
    CancellationToken cancellationToken) =>
{
    var room = await db.Rooms.AsNoTracking().FirstOrDefaultAsync(x => x.RoomId == roomId, cancellationToken);
    if (room is null || room.HostPlayerId is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Room was not found."));
    }

    if (!TokenService.FixedTimeEquals(room.HostTokenHash, TokenService.Hash(hostToken)))
    {
        return Results.Json(new ApiError("FORBIDDEN", "Host token is invalid."), statusCode: 403);
    }

    var tickets = nat.GetHostTickets(roomId, room.HostPlayerId.Value)
        .Select(ToPunchTicketResponse)
        .ToList();
    return Results.Ok(new ListPunchTicketsResponse(tickets));
});

app.MapPost("/v1/rooms/{roomId:guid}/punch-tickets/{ticketId:guid}/complete", (
    Guid roomId,
    Guid ticketId,
    NatTraversalStore nat) =>
{
    nat.Complete(ticketId);
    return Results.Ok(new { ok = true });
});

app.MapPost("/v1/rooms/{roomId:guid}/heartbeat", async (
    Guid roomId,
    RoomHeartbeatRequest request,
    MatchServerDbContext db,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var room = await db.Rooms.FirstOrDefaultAsync(x => x.RoomId == roomId, cancellationToken);
    if (room is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Room was not found."));
    }

    if (!TokenService.FixedTimeEquals(room.HostTokenHash, TokenService.Hash(request.HostToken)))
    {
        return Results.Json(new ApiError("FORBIDDEN", "Host token is invalid."), statusCode: 403);
    }

    if (room.State is RoomState.Ended or RoomState.Expired)
    {
        return Results.Json(new ApiError("ROOM_ENDED", "Room has already ended."), statusCode: 409);
    }

    room.PlayerCount = Math.Clamp(request.PlayerCount, 0, room.MaxPlayers);
    room.ServerState = string.IsNullOrWhiteSpace(request.ServerState) ? room.ServerState : request.ServerState;
    room.LastSeenAt = DateTimeOffset.UtcNow;
    await db.SaveChangesAsync(cancellationToken);
    return Results.Ok(new RoomHeartbeatResponse(true, options.Value.HeartbeatSeconds));
});

app.MapPost("/v1/rooms/{roomId:guid}/start", async (
    Guid roomId,
    RoomLifecycleRequest request,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var result = await UpdateRoomLifecycleAsync(db, roomId, request.HostToken, RoomState.InGame, null, cancellationToken);
    return result;
});

app.MapPost("/v1/rooms/{roomId:guid}/end", async (
    Guid roomId,
    RoomLifecycleRequest request,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var result = await UpdateRoomLifecycleAsync(db, roomId, request.HostToken, RoomState.Ended, "host_ended", cancellationToken);
    return result;
});

app.MapPost("/v1/matchmaking/tickets", async (
    CreateMatchTicketRequest request,
    HttpContext http,
    MatchServerDbContext db,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var now = DateTimeOffset.UtcNow;
    if (request.CanHost)
    {
        if (request.ProbeId is null)
        {
            return BadRequest("HOST_PROBE_REQUIRED", "probeId is required when canHost is true.");
        }

        var probeOk = await db.HostProbes.AnyAsync(x =>
            x.ProbeId == request.ProbeId &&
            x.PlayerId == player.PlayerId &&
            x.Status == HostProbeStatus.Succeeded &&
            x.ExpiresAt > now, cancellationToken);
        if (!probeOk)
        {
            return Results.Json(new ApiError("HOST_PROBE_REQUIRED", "A fresh successful host probe is required for host-capable matchmaking."), statusCode: 409);
        }
    }

    var ticket = new MatchTicket
    {
        TicketId = Guid.NewGuid(),
        PlayerId = player.PlayerId,
        Region = NormalizeText(request.Region, "CN", 16),
        Map = string.IsNullOrWhiteSpace(request.Map) ? null : request.Map.Trim(),
        Mode = string.IsNullOrWhiteSpace(request.Mode) ? null : request.Mode.Trim(),
        Version = NormalizeText(request.Version, "dev", 32),
        CanHost = request.CanHost,
        ProbeId = request.ProbeId,
        RoomName = request.RoomName,
        MaxPlayers = Math.Clamp(request.MaxPlayers, 1, 128),
        State = MatchTicketState.Waiting,
        CreatedAt = now,
        ExpiresAt = now.AddSeconds(options.Value.MatchTicketSeconds)
    };

    db.MatchTickets.Add(ticket);
    await db.SaveChangesAsync(cancellationToken);
    return Results.Ok(new CreateMatchTicketResponse(ticket.TicketId, ticket.State, ticket.ExpiresAt));
});

app.MapGet("/v1/matchmaking/tickets/{ticketId:guid}", async (
    Guid ticketId,
    HttpContext http,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var ticket = await db.MatchTickets.AsNoTracking().FirstOrDefaultAsync(x => x.TicketId == ticketId && x.PlayerId == player.PlayerId, cancellationToken);
    if (ticket is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Match ticket was not found."));
    }

    RoomSummary? roomSummary = null;
    string? connect = null;
    if (ticket.AssignedRoomId is not null)
    {
        var room = await db.Rooms.AsNoTracking().FirstOrDefaultAsync(x => x.RoomId == ticket.AssignedRoomId, cancellationToken);
        if (room is not null)
        {
            roomSummary = EndpointHelpers.ToSummary(room);
            connect = room.Endpoint;
        }
    }

    return Results.Ok(new MatchTicketResponse(
        ticket.TicketId,
        ticket.State,
        ticket.AssignedRoomId,
        roomSummary,
        ticket.HostTokenPlain,
        connect,
        ticket.JoinTicketPlain,
        ticket.FailureReason,
        ticket.ExpiresAt));
});

app.MapDelete("/v1/matchmaking/tickets/{ticketId:guid}", async (
    Guid ticketId,
    HttpContext http,
    MatchServerDbContext db,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var updated = await db.MatchTickets
        .Where(x => x.TicketId == ticketId && x.PlayerId == player.PlayerId && x.State == MatchTicketState.Waiting)
        .ExecuteUpdateAsync(x => x
            .SetProperty(p => p.State, MatchTicketState.Canceled)
            .SetProperty(p => p.FailureReason, "canceled"), cancellationToken);

    return Results.Ok(new { ok = true, updated });
});

app.MapPost("/server/status", async (
    LegacyServerStatusRequest request,
    HttpContext http,
    MatchServerDbContext db,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    if (request.RoomId is not null && !string.IsNullOrWhiteSpace(request.HostToken))
    {
        var room = await db.Rooms.FirstOrDefaultAsync(x => x.RoomId == request.RoomId, cancellationToken);
        if (room is not null && TokenService.FixedTimeEquals(room.HostTokenHash, TokenService.Hash(request.HostToken)))
        {
            room.PlayerCount = Math.Clamp(request.PlayerCount, 0, room.MaxPlayers);
            room.ServerState = request.ServerState;
            room.LastSeenAt = DateTimeOffset.UtcNow;
            await db.SaveChangesAsync(cancellationToken);
            return Results.Ok(new LegacyServerStatusResponse(true, room.RoomId, options.Value.HeartbeatSeconds));
        }
    }

    var remoteIp = EndpointHelpers.GetPublicIp(http);
    var sourceKey = $"{remoteIp}:{request.Port}";
    var endpoint = sourceKey;
    var now = DateTimeOffset.UtcNow;
    var server = await db.LegacyServers.FirstOrDefaultAsync(x => x.SourceKey == sourceKey, cancellationToken);
    if (server is null)
    {
        server = new LegacyServer { ServerId = Guid.NewGuid(), SourceKey = sourceKey };
        db.LegacyServers.Add(server);
    }

    server.Name = NormalizeText(request.Name, "Legacy Server", 64);
    server.Region = NormalizeText(request.Region, "CN", 16);
    server.Mode = NormalizeText(request.Mode, "unknown", 256);
    server.Map = NormalizeText(request.Map, "unknown", 64);
    server.Port = request.Port;
    server.Endpoint = endpoint;
    server.PlayerCount = Math.Clamp(request.PlayerCount, 0, 128);
    server.ServerState = NormalizeText(request.ServerState, "Unknown", 64);
    server.Version = request.Version;
    server.LastSeenAt = now;
    await db.SaveChangesAsync(cancellationToken);

    return Results.Ok(new LegacyServerStatusResponse(true, server.ServerId, options.Value.HeartbeatSeconds));
});

app.MapGet("/v1/servers", async (MatchServerDbContext db, CancellationToken cancellationToken) =>
{
    var cutoff = DateTimeOffset.UtcNow.AddSeconds(-45);
    var items = await db.LegacyServers.AsNoTracking()
        .Where(x => x.LastSeenAt > cutoff)
        .OrderByDescending(x => x.LastSeenAt)
        .Select(x => new
        {
            serverId = x.ServerId,
            name = x.Name,
            region = x.Region,
            mode = x.Mode,
            map = x.Map,
            endpoint = x.Endpoint,
            port = x.Port,
            playerCount = x.PlayerCount,
            serverState = x.ServerState,
            status = "online",
            lastSeenAt = x.LastSeenAt
        })
        .ToListAsync(cancellationToken);
    return Results.Ok(new { items, page = 1, pageSize = items.Count, total = items.Count });
});

app.MapPost("/v1/rooms/{roomId:guid}/host-migration/{**rest}", () =>
    Results.Json(new ApiError("NOT_IMPLEMENTED", "Host migration is reserved for a future version."), statusCode: 501));

app.MapPost("/v1/relay/allocations", async (
    CreateRelayAllocationRequest request,
    HttpContext http,
    MatchServerDbContext db,
    RelayStore relay,
    Microsoft.Extensions.Options.IOptions<MatchServerOptions> options,
    CancellationToken cancellationToken) =>
{
    var player = await http.GetBearerPlayerAsync(db);
    if (player is null)
    {
        return AuthExtensions.UnauthorizedError();
    }

    var role = NormalizeText(request.Role, "", 16).ToLowerInvariant();
    if (role is not "host" and not "client")
    {
        return BadRequest("VALIDATION_ERROR", "role must be host or client.");
    }

    var room = await db.Rooms.AsNoTracking().FirstOrDefaultAsync(x => x.RoomId == request.RoomId, cancellationToken);
    if (room is null || room.HostPlayerId is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Room was not found."));
    }

    if (!RoomOperations.IsJoinable(room))
    {
        return await EndpointHelpers.NotFoundRoomAsync(request.RoomId, db);
    }

    var expiresAt = DateTimeOffset.UtcNow.AddSeconds(options.Value.RelayAllocationSeconds);
    RelayAllocation allocation;
    if (role == "host")
    {
        if (room.HostPlayerId != player.PlayerId)
        {
            return Results.Json(new ApiError("FORBIDDEN", "Only the host player can allocate a host relay endpoint."), statusCode: 403);
        }

        if (string.IsNullOrWhiteSpace(request.HostToken) ||
            !TokenService.FixedTimeEquals(room.HostTokenHash, TokenService.Hash(request.HostToken)))
        {
            return Results.Json(new ApiError("FORBIDDEN", "Host token is invalid."), statusCode: 403);
        }

        allocation = relay.CreateHostAllocation(room.RoomId, player.PlayerId, expiresAt);
    }
    else
    {
        if (string.IsNullOrWhiteSpace(request.JoinTicket))
        {
            return Results.Json(new ApiError("JOIN_TICKET_REQUIRED", "A fresh join ticket is required before allocating relay."), statusCode: 409);
        }

        var now = DateTimeOffset.UtcNow;
        var joinTicketHash = TokenService.Hash(request.JoinTicket);
        var reservation = await db.RoomPlayers.AsNoTracking().FirstOrDefaultAsync(x =>
            x.RoomId == room.RoomId &&
            x.PlayerId == player.PlayerId &&
            x.JoinTicketHash == joinTicketHash &&
            (x.Status == RoomPlayerStatus.Reserved || x.Status == RoomPlayerStatus.Joined) &&
            x.ExpiresAt > now, cancellationToken);
        if (reservation is null)
        {
            return Results.Json(new ApiError("JOIN_TICKET_REQUIRED", "A fresh join ticket is required before allocating relay."), statusCode: 409);
        }

        allocation = relay.CreateClientAllocation(room.RoomId, room.HostPlayerId.Value, player.PlayerId, expiresAt);
    }

    return Results.Ok(new CreateRelayAllocationResponse(
        allocation.SessionId,
        allocation.Role,
        allocation.Secret,
        http.Request.Host.Host,
        options.Value.UdpRelayPort,
        allocation.ExpiresAt));
});

await app.RunAsync();

static IResult BadRequest(string code, string message)
{
    return Results.BadRequest(new ApiError(code, message));
}

static string NormalizeText(string? value, string fallback, int maxLength)
{
    var text = string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
    return text.Length <= maxLength ? text : text[..maxLength];
}

static async Task<IResult> UpdateRoomLifecycleAsync(
    MatchServerDbContext db,
    Guid roomId,
    string hostToken,
    RoomState nextState,
    string? endedReason,
    CancellationToken cancellationToken)
{
    var room = await db.Rooms.FirstOrDefaultAsync(x => x.RoomId == roomId, cancellationToken);
    if (room is null)
    {
        return Results.NotFound(new ApiError("NOT_FOUND", "Room was not found."));
    }

    if (!TokenService.FixedTimeEquals(room.HostTokenHash, TokenService.Hash(hostToken)))
    {
        return Results.Json(new ApiError("FORBIDDEN", "Host token is invalid."), statusCode: 403);
    }

    room.State = nextState;
    room.EndedReason = endedReason;
    room.LastSeenAt = DateTimeOffset.UtcNow;
    await db.SaveChangesAsync(cancellationToken);
    return Results.Ok(EndpointHelpers.ToSummary(room));
}

static PunchTicketResponse ToPunchTicketResponse(PunchTicket ticket)
{
    return new PunchTicketResponse(
        ticket.TicketId,
        ticket.State,
        ticket.Nonce,
        ticket.HostEndpoint,
        ticket.HostLocalEndpoint,
        ticket.ClientEndpoint,
        ticket.ClientLocalEndpoint,
        ticket.ExpiresAt);
}
