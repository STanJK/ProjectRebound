using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using ProjectRebound.Contracts;

namespace ProjectRebound.Browser.Services;

public sealed class ApiClient
{
    private readonly HttpClient _http = new();
    private readonly JsonSerializerOptions _jsonOptions = new(JsonSerializerDefaults.Web);

    public ApiClient()
    {
        _jsonOptions.Converters.Add(new JsonStringEnumConverter());
    }

    public void Configure(string backendUrl, string accessToken)
    {
        var normalized = backendUrl.EndsWith('/') ? backendUrl : backendUrl + "/";
        _http.BaseAddress = new Uri(normalized);
        _http.DefaultRequestHeaders.Authorization = string.IsNullOrWhiteSpace(accessToken)
            ? null
            : new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", accessToken);
    }

    public async Task<GuestAuthResponse> LoginGuestAsync(GuestAuthRequest request)
    {
        return await PostAsync<GuestAuthRequest, GuestAuthResponse>("v1/auth/guest", request);
    }

    public async Task<CreateHostProbeResponse> CreateHostProbeAsync(int port)
    {
        return await PostAsync<CreateHostProbeRequest, CreateHostProbeResponse>("v1/host-probes", new CreateHostProbeRequest(port));
    }

    public async Task<HostProbeResponse> ConfirmHostProbeAsync(Guid probeId, string nonce)
    {
        return await PostAsync<ConfirmHostProbeRequest, HostProbeResponse>($"v1/host-probes/{probeId}/confirm", new ConfirmHostProbeRequest(nonce));
    }

    public async Task<CreateRoomResponse> CreateRoomAsync(CreateRoomRequest request)
    {
        return await PostAsync<CreateRoomRequest, CreateRoomResponse>("v1/rooms", request);
    }

    public async Task<PagedRoomsResponse> GetRoomsAsync(string region, string version, string? state = null)
    {
        var query = $"v1/rooms?region={Uri.EscapeDataString(region)}&version={Uri.EscapeDataString(version)}";
        if (!string.IsNullOrWhiteSpace(state))
        {
            query += $"&state={Uri.EscapeDataString(state)}";
        }

        return await GetAsync<PagedRoomsResponse>(query);
    }

    public async Task<RoomSummary> GetRoomAsync(Guid roomId)
    {
        return await GetAsync<RoomSummary>($"v1/rooms/{roomId}");
    }

    public async Task<JoinRoomResponse> JoinRoomAsync(Guid roomId, string version)
    {
        return await PostAsync<JoinRoomRequest, JoinRoomResponse>($"v1/rooms/{roomId}/join", new JoinRoomRequest(version));
    }

    public async Task<CreateMatchTicketResponse> CreateMatchTicketAsync(CreateMatchTicketRequest request)
    {
        return await PostAsync<CreateMatchTicketRequest, CreateMatchTicketResponse>("v1/matchmaking/tickets", request);
    }

    public async Task<MatchTicketResponse> GetMatchTicketAsync(Guid ticketId)
    {
        return await GetAsync<MatchTicketResponse>($"v1/matchmaking/tickets/{ticketId}");
    }

    private async Task<T> GetAsync<T>(string path)
    {
        using var response = await _http.GetAsync(path);
        return await ReadResponseAsync<T>(response);
    }

    private async Task<TResponse> PostAsync<TRequest, TResponse>(string path, TRequest request)
    {
        using var response = await _http.PostAsJsonAsync(path, request, _jsonOptions);
        return await ReadResponseAsync<TResponse>(response);
    }

    private async Task<T> ReadResponseAsync<T>(HttpResponseMessage response)
    {
        if (response.IsSuccessStatusCode)
        {
            var value = await response.Content.ReadFromJsonAsync<T>(_jsonOptions);
            return value ?? throw new InvalidOperationException("Server returned an empty response.");
        }

        var raw = await response.Content.ReadAsStringAsync();
        try
        {
            var apiError = JsonSerializer.Deserialize<ApiError>(raw, _jsonOptions);
            if (apiError is not null)
            {
                throw new InvalidOperationException($"{apiError.Code}: {apiError.Message}");
            }
        }
        catch (JsonException)
        {
        }

        throw new InvalidOperationException($"HTTP {(int)response.StatusCode}: {raw}");
    }
}
