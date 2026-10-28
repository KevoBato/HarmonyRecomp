namespace HarmonyLauncher.Core;

// Where everything lives. The launcher drives a HarmonyRecomp source folder
// (the one with scripts/build.py); like build.py and toolchain.py expect, the
// ReXGlue SDK and the portable tools go NEXT TO it, never inside:
//
//   <launcher folder>/
//     HarmonyLauncher.exe
//     HarmonyRecomp/          source, game/, dlc/, out/ (the built game)
//     rexglue-sdk/            cloned and built by build.py
//     harmonyrecomp-tools/    portable LLVM, CMake, ..., Python
public sealed class Layout
{
    public const string Preset = "win-amd64-release";

    public Layout(string source) => Source = Path.GetFullPath(source);

    public string Source { get; }
    public string Parent => Path.GetDirectoryName(Source)!;
    public string ToolsDir =>
        Environment.GetEnvironmentVariable("HARMONYRECOMP_TOOLS") is { Length: > 0 } env
            ? Path.GetFullPath(env)
            : Path.Combine(Parent, "harmonyrecomp-tools");
    public string Python => Path.Combine(ToolsDir, "python", "python.exe");
    public string LockFile => Path.Combine(Source, "toolchain.lock.json");
    public string GameDir => Path.Combine(Source, "game");
    public string DlcDir => Path.Combine(Source, "dlc");
    public string OutDir => Path.Combine(Source, "out", "build", Preset);
    public string GameExe => Path.Combine(OutDir, "harmonyrecomp.exe");
    public string RuntimeConfig => Path.Combine(OutDir, "harmonyrecomp.toml");
    public string LogsDir => Path.Combine(OutDir, "logs");

    public static string LauncherDir => AppContext.BaseDirectory;
    public static string DefaultSource => Path.Combine(LauncherDir, "HarmonyRecomp");

    public static bool IsSource(string dir) =>
        File.Exists(Path.Combine(dir, "scripts", "build.py")) &&
        File.Exists(Path.Combine(dir, "toolchain.lock.json"));

    // --source, then a checkout the launcher sits inside (launcher/bin/... during
    // development), then HarmonyRecomp/ next to the exe. Null: needs a download.
    public static Layout? Find()
    {
        if (Program.SourceOverride is { } overridden)
            return IsSource(overridden) ? new Layout(overridden) : null;
        for (var dir = new DirectoryInfo(LauncherDir); dir != null; dir = dir.Parent)
            if (IsSource(dir.FullName))
                return new Layout(dir.FullName);
        return IsSource(DefaultSource) ? new Layout(DefaultSource) : null;
    }
}
