using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace HarmonyLauncher.Core;

// The two things the launcher downloads itself; everything else is done by
// scripts/toolchain.py, which runs on the Python fetched here.
public static class Downloads
{
    static readonly HttpClient Http = CreateClient();

    // Source used when the launcher is not next to a HarmonyRecomp folder.
    public const string SourceZipUrl = "https://github.com/Wrexxial/HarmonyRecomp/archive/refs/heads/main.zip";

    static HttpClient CreateClient()
    {
        var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HarmonyLauncher/0.1");
        return http;
    }

    // Downloads the repository archive and unpacks it into target, without
    // the archive's top folder (HarmonyRecomp-main/).
    public static async Task DownloadSourceAsync(string target, IProgress<double> progress, CancellationToken ct)
    {
        var zip = Path.Combine(Path.GetTempPath(), $"harmonyrecomp-source-{Guid.NewGuid():N}.zip");
        try
        {
            await DownloadAsync(SourceZipUrl, zip, null, progress, ct);
            using var archive = ZipFile.OpenRead(zip);
            var root = Path.GetFullPath(target);
            foreach (var entry in archive.Entries)
            {
                int slash = entry.FullName.IndexOf('/');
                var relative = slash >= 0 ? entry.FullName[(slash + 1)..] : "";
                if (relative.Length == 0 || relative.EndsWith('/'))
                    continue;
                var dest = Path.GetFullPath(Path.Combine(root, relative));
                if (!dest.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"Unsafe path in the source archive: {entry.FullName}");
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                entry.ExtractToFile(dest, overwrite: true);
            }
        }
        finally
        {
            File.Delete(zip);
        }
    }

    // Fetches the embeddable Python pinned in toolchain.lock.json (the entry
    // marked "launcher"), with the same stamp file toolchain.py writes.
    public static async Task EnsurePythonAsync(Layout layout, IProgress<double> progress, CancellationToken ct)
    {
        using var doc = JsonDocument.Parse(await File.ReadAllTextAsync(layout.LockFile, ct));
        var entry = doc.RootElement.GetProperty("tools").EnumerateArray()
            .First(e => e.TryGetProperty("launcher", out var l) && l.GetBoolean());
        string name = entry.GetProperty("name").GetString()!;
        string sha256 = entry.GetProperty("sha256").GetString()!.ToLowerInvariant();
        string dest = Path.Combine(layout.ToolsDir, entry.GetProperty("dest").GetString()!);
        string stamp = Path.Combine(dest, $".harmonyrecomp-tool-{name}");
        if (File.Exists(layout.Python) && File.Exists(stamp) && File.ReadAllText(stamp).Trim() == sha256)
            return;

        var downloads = Path.Combine(layout.ToolsDir, "downloads");
        Directory.CreateDirectory(downloads);
        var archive = Path.Combine(downloads, name);
        await DownloadAsync(entry.GetProperty("url").GetString()!, archive, sha256, progress, ct);
        Directory.CreateDirectory(dest);
        ZipFile.ExtractToDirectory(archive, dest, overwriteFiles: true);
        await File.WriteAllTextAsync(stamp, sha256 + "\n", ct);
    }

    static async Task DownloadAsync(string url, string target, string? sha256, IProgress<double> progress, CancellationToken ct)
    {
        var partial = target + ".partial";
        using (var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct))
        {
            response.EnsureSuccessStatusCode();
            long? total = response.Content.Headers.ContentLength;
            await using var src = await response.Content.ReadAsStreamAsync(ct);
            await using var dst = File.Create(partial);
            var buffer = new byte[1 << 16];
            long done = 0;
            int read;
            while ((read = await src.ReadAsync(buffer, ct)) > 0)
            {
                await dst.WriteAsync(buffer.AsMemory(0, read), ct);
                done += read;
                if (total > 0)
                    progress.Report((double)done / total.Value);
            }
        }
        if (sha256 != null)
        {
            await using var f = File.OpenRead(partial);
            var actual = Convert.ToHexStringLower(await SHA256.HashDataAsync(f, ct));
            if (actual != sha256)
            {
                File.Delete(partial);
                throw new InvalidDataException($"{Path.GetFileName(target)} failed its checksum check; try again.");
            }
        }
        File.Move(partial, target, overwrite: true);
    }
}
