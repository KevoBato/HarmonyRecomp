using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;

namespace HarmonyLauncher.Core;

// Runs scripts/toolchain.py and scripts/build.py with the portable Python and
// turns their output into one progress value for the progress bar.
public sealed partial class BuildRunner
{
    readonly Layout layout;
    readonly Action<string> onLine;

    public BuildRunner(Layout layout, Action<string> onLine)
    {
        this.layout = layout;
        this.onLine = onLine;
    }

    public double Progress { get; private set; }
    public string Status { get; private set; } = "";

    public async Task<int> RunPythonAsync(string script, string[] args, CancellationToken ct)
    {
        var psi = new ProcessStartInfo(layout.Python)
        {
            WorkingDirectory = layout.Source,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        psi.ArgumentList.Add("-u");
        psi.ArgumentList.Add(Path.Combine("scripts", script));
        foreach (var a in args)
            psi.ArgumentList.Add(a);
        psi.Environment["PYTHONUNBUFFERED"] = "1";
        psi.Environment["PYTHONIOENCODING"] = "utf-8";
        psi.Environment["HARMONYRECOMP_TOOLS"] = layout.ToolsDir;

        using var process = new Process { StartInfo = psi };
        process.OutputDataReceived += (_, e) => { if (e.Data != null) Feed(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) Feed(e.Data); };
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        using var reg = ct.Register(() =>
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
        });
        await process.WaitForExitAsync(CancellationToken.None);
        ct.ThrowIfCancellationRequested();
        return process.ExitCode;
    }

    // Overall progress: the tools take the first 25 %, then build.py's five
    // steps share the rest by how long they usually take (the SDK build most).
    static readonly (double Start, double End)[] Steps =
    [
        (0.25, 0.27),  // 1 extract the game
        (0.27, 0.32),  // 2 clone the SDK
        (0.32, 0.78),  // 3 build the SDK
        (0.78, 0.88),  // 4 codegen
        (0.88, 1.00),  // 5 build the game
    ];
    int step;

    [GeneratedRegex(@"^\[tools\] step (\d+)/(\d+): (.+)$")]
    private static partial Regex ToolStep();
    [GeneratedRegex(@"^\[tools\] progress (\S+) (\d+) (\d+)$")]
    private static partial Regex ToolProgress();
    [GeneratedRegex(@"^\[([1-5])/5\] (.+)$")]
    private static partial Regex BuildStep();
    [GeneratedRegex(@"^\[(\d+)/(\d+)\] ")]
    private static partial Regex NinjaProgress();

    int toolStep, toolSteps = 1;

    void Feed(string line)
    {
        Match m;
        if ((m = ToolProgress().Match(line)).Success)
        {
            // Not shown in the log: one line per second per download.
            double part = double.Parse(m.Groups[2].Value) / Math.Max(1, double.Parse(m.Groups[3].Value));
            Progress = 0.25 * (toolStep - 1 + part) / toolSteps;
            return;
        }
        if ((m = ToolStep().Match(line)).Success)
        {
            toolStep = int.Parse(m.Groups[1].Value);
            toolSteps = int.Parse(m.Groups[2].Value);
            Progress = 0.25 * (toolStep - 1) / toolSteps;
            Status = $"Downloading build tools ({toolStep}/{toolSteps}): {m.Groups[3].Value}";
        }
        else if (line.StartsWith("[tools] all build tools are ready"))
        {
            Progress = 0.25;
            Status = "Build tools ready";
        }
        else if ((m = BuildStep().Match(line)).Success)
        {
            step = int.Parse(m.Groups[1].Value);
            Progress = Steps[step - 1].Start;
            Status = $"Step {step}/5: {m.Groups[2].Value}";
        }
        else if (step is 3 or 5 && (m = NinjaProgress().Match(line)).Success)
        {
            double part = double.Parse(m.Groups[1].Value) / Math.Max(1, double.Parse(m.Groups[2].Value));
            var (start, end) = Steps[step - 1];
            Progress = start + (end - start) * part;
        }
        else if (line.StartsWith("Done. Output is in"))
        {
            Progress = 1;
            Status = "Build finished";
        }
        onLine(line);
    }
}
