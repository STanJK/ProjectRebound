using System.Diagnostics;
using System.IO;
using ProjectRebound.Contracts;

namespace ProjectRebound.Browser.Services;

public sealed class GameLauncher
{
    public Process StartClient(string gameDirectory, string connect)
    {
        var exe = FindRequiredFile(gameDirectory, "ProjectBoundarySteam-Win64-Shipping.exe");
        var startInfo = new ProcessStartInfo(exe)
        {
            WorkingDirectory = Path.GetDirectoryName(exe) ?? gameDirectory,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add($"-match={connect}");
        return Process.Start(startInfo) ?? throw new InvalidOperationException("Failed to start game client.");
    }

    public Process StartHost(
        string gameDirectory,
        string backendUrl,
        RoomSummary room,
        string hostToken)
    {
        var wrapper = FindFile(gameDirectory, "ProjectReboundServerWrapper.exe");
        if (wrapper is not null)
        {
            var startInfo = new ProcessStartInfo(wrapper)
            {
                WorkingDirectory = Path.GetDirectoryName(wrapper) ?? gameDirectory,
                UseShellExecute = false
            };

            startInfo.ArgumentList.Add($"-online={BackendForGame(backendUrl)}");
            startInfo.ArgumentList.Add($"-roomid={room.RoomId}");
            startInfo.ArgumentList.Add($"-hosttoken={hostToken}");
            startInfo.ArgumentList.Add($"-map={room.Map}");
            startInfo.ArgumentList.Add($"-mode={room.Mode}");
            startInfo.ArgumentList.Add($"-servername={SanitizeCommandValue(room.Name)}");
            startInfo.ArgumentList.Add($"-serverregion={room.Region}");
            startInfo.ArgumentList.Add($"-port={room.Port}");
            return Process.Start(startInfo) ?? throw new InvalidOperationException("Failed to start server wrapper.");
        }

        var exe = FindRequiredFile(gameDirectory, "ProjectBoundarySteam-Win64-Shipping.exe");
        var direct = new ProcessStartInfo(exe)
        {
            WorkingDirectory = Path.GetDirectoryName(exe) ?? gameDirectory,
            UseShellExecute = false
        };
        direct.ArgumentList.Add("-log");
        direct.ArgumentList.Add("-server");
        direct.ArgumentList.Add("-nullrhi");
        direct.ArgumentList.Add($"-online={BackendForGame(backendUrl)}");
        direct.ArgumentList.Add($"-roomid={room.RoomId}");
        direct.ArgumentList.Add($"-hosttoken={hostToken}");
        direct.ArgumentList.Add($"-map={room.Map}");
        direct.ArgumentList.Add($"-mode={ModeToPath(room.Mode)}");
        direct.ArgumentList.Add($"-servername={SanitizeCommandValue(room.Name)}");
        direct.ArgumentList.Add($"-serverregion={room.Region}");
        direct.ArgumentList.Add($"-port={room.Port}");
        if (string.Equals(room.Mode, "pve", StringComparison.OrdinalIgnoreCase))
        {
            direct.ArgumentList.Add("-pve");
        }

        return Process.Start(direct) ?? throw new InvalidOperationException("Failed to start host game.");
    }

    private static string BackendForGame(string backendUrl)
    {
        if (Uri.TryCreate(backendUrl, UriKind.Absolute, out var uri))
        {
            return uri.IsDefaultPort ? uri.Host : $"{uri.Host}:{uri.Port}";
        }

        return backendUrl.Replace("http://", "", StringComparison.OrdinalIgnoreCase)
            .Replace("https://", "", StringComparison.OrdinalIgnoreCase)
            .TrimEnd('/');
    }

    private static string ModeToPath(string mode)
    {
        return string.Equals(mode, "pvp", StringComparison.OrdinalIgnoreCase)
            ? "/Game/Online/GameMode/PBGameMode_Rush_BP.PBGameMode_Rush_BP_C"
            : "/Game/Online/GameMode/BP_PBGameMode_Rush_PVE_Normal.BP_PBGameMode_Rush_PVE_Normal_C";
    }

    private static string SanitizeCommandValue(string value)
    {
        return value.Replace(' ', '_').Replace('\t', '_');
    }

    private static string FindRequiredFile(string root, string fileName)
    {
        return FindFile(root, fileName) ?? throw new FileNotFoundException($"Could not find {fileName} under {root}.");
    }

    private static string? FindFile(string root, string fileName)
    {
        if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root))
        {
            return null;
        }

        var direct = Path.Combine(root, fileName);
        if (File.Exists(direct))
        {
            return direct;
        }

        return Directory.EnumerateFiles(root, fileName, SearchOption.AllDirectories).FirstOrDefault();
    }
}
