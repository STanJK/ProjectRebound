using System.IO;
using System.Text.Json;
using ProjectRebound.Browser.Models;

namespace ProjectRebound.Browser.Services;

public sealed class ConfigStore
{
    private readonly JsonSerializerOptions _jsonOptions = new() { WriteIndented = true };

    public string ConfigPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "ProjectReboundBrowser",
        "config.json");

    public async Task<AppConfig> LoadAsync()
    {
        if (!File.Exists(ConfigPath))
        {
            return new AppConfig();
        }

        await using var stream = File.OpenRead(ConfigPath);
        return await JsonSerializer.DeserializeAsync<AppConfig>(stream) ?? new AppConfig();
    }

    public async Task SaveAsync(AppConfig config)
    {
        var directory = Path.GetDirectoryName(ConfigPath);
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
        }

        await using var stream = File.Create(ConfigPath);
        await JsonSerializer.SerializeAsync(stream, config, _jsonOptions);
    }
}
