using System.Text.Json;

namespace HarmonyLauncher.Core;

// The launcher's own choices, kept in launcher.json next to the exe.
public sealed class LauncherConfig
{
    public string? GamePackage { get; set; }
    public List<string> DlcPackages { get; set; } = [];
    public bool DlcEnabled { get; set; }

    static string FilePath => Path.Combine(Layout.LauncherDir, "launcher.json");
    static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static LauncherConfig Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<LauncherConfig>(File.ReadAllText(FilePath)) ?? new();
        }
        catch (JsonException) { }
        return new();
    }

    public void Save()
    {
        try { File.WriteAllText(FilePath, JsonSerializer.Serialize(this, Options)); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
