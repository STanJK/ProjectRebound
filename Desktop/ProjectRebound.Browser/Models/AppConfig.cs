namespace ProjectRebound.Browser.Models;

public sealed class AppConfig
{
    public string BackendUrl { get; set; } = "http://127.0.0.1:5000";
    public string GameDirectory { get; set; } = "";
    public string DisplayName { get; set; } = Environment.UserName;
    public string Region { get; set; } = "CN";
    public string Version { get; set; } = "dev";
    public int Port { get; set; } = 7777;
    public string AccessToken { get; set; } = "";
}
