using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Windows.Forms;
using System.Windows.Input;
using ProjectRebound.Browser.Models;
using ProjectRebound.Browser.Services;
using ProjectRebound.Contracts;

namespace ProjectRebound.Browser.ViewModels;

public sealed class MainViewModel : INotifyPropertyChanged
{
    private readonly ConfigStore _configStore = new();
    private readonly ApiClient _api = new();
    private readonly UdpProbeListener _udpProbeListener = new();
    private readonly GameLauncher _gameLauncher = new();
    private AppConfig _config = new();
    private RoomSummary? _selectedRoom;
    private string _status = "Ready.";
    private string _roomName = "ProjectRebound Room";
    private string _map = "Warehouse";
    private string _mode = "pve";
    private int _maxPlayers = 8;

    public event PropertyChangedEventHandler? PropertyChanged;

    public ObservableCollection<RoomSummary> Rooms { get; } = [];

    public ICommand SaveCommand { get; }
    public ICommand BrowseGameDirectoryCommand { get; }
    public ICommand RefreshCommand { get; }
    public ICommand CreateRoomCommand { get; }
    public ICommand JoinRoomCommand { get; }
    public ICommand QuickMatchCommand { get; }

    public MainViewModel()
    {
        SaveCommand = new AsyncRelayCommand(SaveAsync);
        BrowseGameDirectoryCommand = new RelayCommand(BrowseGameDirectory);
        RefreshCommand = new AsyncRelayCommand(RefreshRoomsAsync);
        CreateRoomCommand = new AsyncRelayCommand(CreateRoomAsync);
        JoinRoomCommand = new AsyncRelayCommand(JoinSelectedRoomAsync, () => SelectedRoom is not null);
        QuickMatchCommand = new AsyncRelayCommand(QuickMatchAsync);
    }

    public string BackendUrl
    {
        get => _config.BackendUrl;
        set { _config.BackendUrl = value; OnPropertyChanged(); }
    }

    public string GameDirectory
    {
        get => _config.GameDirectory;
        set { _config.GameDirectory = value; OnPropertyChanged(); }
    }

    public string DisplayName
    {
        get => _config.DisplayName;
        set { _config.DisplayName = value; OnPropertyChanged(); }
    }

    public string Region
    {
        get => _config.Region;
        set { _config.Region = value; OnPropertyChanged(); }
    }

    public string Version
    {
        get => _config.Version;
        set { _config.Version = value; OnPropertyChanged(); }
    }

    public int Port
    {
        get => _config.Port;
        set { _config.Port = value; OnPropertyChanged(); }
    }

    public string RoomName
    {
        get => _roomName;
        set { _roomName = value; OnPropertyChanged(); }
    }

    public string Map
    {
        get => _map;
        set { _map = value; OnPropertyChanged(); }
    }

    public string Mode
    {
        get => _mode;
        set { _mode = value; OnPropertyChanged(); }
    }

    public int MaxPlayers
    {
        get => _maxPlayers;
        set { _maxPlayers = value; OnPropertyChanged(); }
    }

    public RoomSummary? SelectedRoom
    {
        get => _selectedRoom;
        set { _selectedRoom = value; OnPropertyChanged(); CommandManager.InvalidateRequerySuggested(); }
    }

    public string Status
    {
        get => _status;
        set { _status = value; OnPropertyChanged(); }
    }

    public async Task InitializeAsync()
    {
        _config = await _configStore.LoadAsync();
        OnPropertyChanged(nameof(BackendUrl));
        OnPropertyChanged(nameof(GameDirectory));
        OnPropertyChanged(nameof(DisplayName));
        OnPropertyChanged(nameof(Region));
        OnPropertyChanged(nameof(Version));
        OnPropertyChanged(nameof(Port));

        await EnsureLoggedInAsync();
        await RefreshRoomsAsync();
    }

    private async Task EnsureLoggedInAsync()
    {
        _api.Configure(BackendUrl, "");
        var auth = await _api.LoginGuestAsync(new GuestAuthRequest(DisplayName, string.IsNullOrWhiteSpace(_config.AccessToken) ? null : _config.AccessToken));
        _config.AccessToken = auth.AccessToken;
        _api.Configure(BackendUrl, _config.AccessToken);
        await _configStore.SaveAsync(_config);
        Status = $"Logged in as {auth.DisplayName}.";
    }

    private async Task SaveAsync()
    {
        await ExecuteAsync("Saving settings...", async () =>
        {
            await _configStore.SaveAsync(_config);
            await EnsureLoggedInAsync();
            Status = "Settings saved.";
        });
    }

    private void BrowseGameDirectory()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select the Project Boundary game directory",
            UseDescriptionForTitle = true,
            SelectedPath = string.IsNullOrWhiteSpace(GameDirectory) ? Environment.CurrentDirectory : GameDirectory
        };

        if (dialog.ShowDialog() == DialogResult.OK)
        {
            GameDirectory = dialog.SelectedPath;
        }
    }

    private async Task RefreshRoomsAsync()
    {
        await ExecuteAsync("Refreshing rooms...", async () =>
        {
            _api.Configure(BackendUrl, _config.AccessToken);
            var rooms = await _api.GetRoomsAsync(Region, Version);
            Rooms.Clear();
            foreach (var room in rooms.Items)
            {
                Rooms.Add(room);
            }

            Status = $"Loaded {rooms.Items.Count} rooms.";
        });
    }

    private async Task CreateRoomAsync()
    {
        await ExecuteAsync("Creating room...", async () =>
        {
            await EnsureGameDirectoryAsync();
            var probe = await RunHostProbeAsync();
            var created = await _api.CreateRoomAsync(new CreateRoomRequest(
                probe.ProbeId,
                RoomName,
                Region,
                Map,
                Mode,
                Version,
                MaxPlayers));

            var room = await _api.GetRoomAsync(created.RoomId);
            _gameLauncher.StartHost(GameDirectory, BackendUrl, room, created.HostToken);
            Status = $"Room {room.Name} created and host launched.";
            await RefreshRoomsAsync();
        });
    }

    private async Task JoinSelectedRoomAsync()
    {
        if (SelectedRoom is null)
        {
            Status = "Select a room first.";
            return;
        }

        await ExecuteAsync("Joining room...", async () =>
        {
            await EnsureGameDirectoryAsync();
            var join = await _api.JoinRoomAsync(SelectedRoom.RoomId, Version);
            _gameLauncher.StartClient(GameDirectory, join.Connect);
            Status = $"Launching client for {join.Connect}.";
        });
    }

    private async Task QuickMatchAsync()
    {
        await ExecuteAsync("Queueing quick match...", async () =>
        {
            await EnsureGameDirectoryAsync();
            var probe = await RunHostProbeAsync();
            var ticket = await _api.CreateMatchTicketAsync(new CreateMatchTicketRequest(
                Region,
                Map,
                Mode,
                Version,
                true,
                probe.ProbeId,
                RoomName,
                MaxPlayers));

            for (var i = 0; i < 70; i++)
            {
                await Task.Delay(TimeSpan.FromSeconds(2));
                var state = await _api.GetMatchTicketAsync(ticket.TicketId);
                Status = $"Matchmaking: {state.State}.";

                if (state.State == MatchTicketState.HostAssigned && state.Room is not null && !string.IsNullOrWhiteSpace(state.HostToken))
                {
                    _gameLauncher.StartHost(GameDirectory, BackendUrl, state.Room, state.HostToken);
                    Status = $"You are host for room {state.Room.Name}.";
                    await RefreshRoomsAsync();
                    return;
                }

                if (state.State == MatchTicketState.Matched && !string.IsNullOrWhiteSpace(state.Connect))
                {
                    _gameLauncher.StartClient(GameDirectory, state.Connect);
                    Status = $"Matched. Launching client for {state.Connect}.";
                    return;
                }

                if (state.State is MatchTicketState.Failed or MatchTicketState.Canceled or MatchTicketState.Expired)
                {
                    Status = $"Matchmaking ended: {state.FailureReason ?? state.State.ToString()}.";
                    return;
                }
            }

            Status = "Matchmaking timed out.";
        });
    }

    private async Task<CreateHostProbeResponse> RunHostProbeAsync()
    {
        _api.Configure(BackendUrl, _config.AccessToken);
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var receiveTask = _udpProbeListener.WaitForNonceAsync(Port, TimeSpan.FromSeconds(15), cts.Token);
        var probe = await _api.CreateHostProbeAsync(Port);
        var receivedNonce = await receiveTask;
        if (!string.Equals(receivedNonce, probe.Nonce, StringComparison.Ordinal))
        {
            throw new InvalidOperationException("UDP probe nonce mismatch.");
        }

        await _api.ConfirmHostProbeAsync(probe.ProbeId, receivedNonce);
        return probe;
    }

    private Task EnsureGameDirectoryAsync()
    {
        if (string.IsNullOrWhiteSpace(GameDirectory) || !Directory.Exists(GameDirectory))
        {
            throw new InvalidOperationException("Set a valid game directory first.");
        }

        return Task.CompletedTask;
    }

    private async Task ExecuteAsync(string startStatus, Func<Task> action)
    {
        try
        {
            Status = startStatus;
            await action();
        }
        catch (Exception ex)
        {
            Status = ex.Message;
        }
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
