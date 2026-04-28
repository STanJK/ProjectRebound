namespace ProjectRebound.Contracts;

public enum PlayerStatus
{
    Active,
    Disabled
}

public enum HostProbeStatus
{
    Pending,
    Succeeded,
    Expired,
    Failed
}

public enum RoomState
{
    Probing,
    Open,
    Starting,
    InGame,
    Ended,
    Expired
}

public enum RoomPlayerStatus
{
    Reserved,
    Joined,
    Left,
    Expired
}

public enum MatchTicketState
{
    Waiting,
    HostAssigned,
    Matched,
    Failed,
    Canceled,
    Expired
}

public enum PunchTicketState
{
    Pending,
    Active,
    Completed,
    Expired,
    Failed
}
