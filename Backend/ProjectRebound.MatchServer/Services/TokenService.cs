using System.Security.Cryptography;
using System.Text;

namespace ProjectRebound.MatchServer.Services;

public static class TokenService
{
    public static string NewToken(int bytes = 32)
    {
        return Convert.ToBase64String(RandomNumberGenerator.GetBytes(bytes))
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');
    }

    public static string Hash(string value)
    {
        var bytes = SHA256.HashData(Encoding.UTF8.GetBytes(value));
        return Convert.ToHexString(bytes);
    }

    public static bool FixedTimeEquals(string leftHash, string rightHash)
    {
        var left = Encoding.UTF8.GetBytes(leftHash);
        var right = Encoding.UTF8.GetBytes(rightHash);
        return left.Length == right.Length && CryptographicOperations.FixedTimeEquals(left, right);
    }
}
