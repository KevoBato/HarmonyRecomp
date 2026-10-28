using System.Text.RegularExpressions;

namespace HarmonyLauncher.Core;

// Edits single "key = value" lines of harmonyrecomp.toml (the runtime settings
// next to the exe) and leaves every other line and comment as it is. A
// commented-out example line ("# key = ...") is replaced in place.
public sealed class RuntimeSettings
{
    readonly string path;
    readonly List<string> lines;

    RuntimeSettings(string path, List<string> lines)
    {
        this.path = path;
        this.lines = lines;
    }

    public static RuntimeSettings Load(string path) =>
        new(path, File.Exists(path) ? File.ReadAllLines(path).ToList() : []);

    static Regex KeyLine(string key) => new($@"^\s*{Regex.Escape(key)}\s*=\s*(.*?)\s*(#.*)?$");
    static Regex CommentedKeyLine(string key) => new($@"^\s*#\s*{Regex.Escape(key)}\s*=");

    string? Raw(string key)
    {
        var re = KeyLine(key);
        foreach (var line in lines)
            if (re.Match(line) is { Success: true } m)
                return m.Groups[1].Value;
        return null;
    }

    public bool GetBool(string key, bool fallback) =>
        Raw(key) is { } v ? v == "true" || v == "1" : fallback;

    public string GetString(string key) =>
        Raw(key) is { } v && v.Length >= 2 && v[0] == '"' && v[^1] == '"'
            ? v[1..^1].Replace("\\\"", "\"").Replace("\\\\", "\\")
            : "";

    public void SetBool(string key, bool value) => SetRaw(key, value ? "true" : "false");
    public void SetInt(string key, int value) => SetRaw(key, value.ToString());
    public void SetString(string key, string value) =>
        SetRaw(key, "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"");

    void SetRaw(string key, string value)
    {
        var line = $"{key} = {value}";
        var re = KeyLine(key);
        for (int i = 0; i < lines.Count; i++)
            if (re.IsMatch(lines[i])) { lines[i] = line; return; }
        var commented = CommentedKeyLine(key);
        for (int i = 0; i < lines.Count; i++)
            if (commented.IsMatch(lines[i])) { lines[i] = line; return; }
        lines.Add(line);
    }

    public void Save() => File.WriteAllLines(path, lines);
}
