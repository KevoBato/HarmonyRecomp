using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using HarmonyLauncher.Core;

namespace HarmonyLauncher;

public sealed class MainWindow : Window
{
    static readonly IBrush Gold = Brush.Parse("#E2C275");
    static readonly IBrush Crimson = Brush.Parse("#A3123A");
    static readonly IBrush CardBg = Brush.Parse("#CC1C1420");
    static readonly IBrush CardBorder = Brush.Parse("#3A2430");
    static readonly IBrush Muted = Brush.Parse("#A89AA6");
    static readonly IBrush Good = Brush.Parse("#7FD08A");
    static readonly IBrush Bad = Brush.Parse("#F07070");
    static readonly IBrush Warn = Brush.Parse("#E8B04A");

    Layout? layout = Layout.Find();
    readonly LauncherConfig config = LauncherConfig.Load();
    CancellationTokenSource? cts;
    BuildRunner? runner;
    double prepProgress;  // source/Python download before the runner starts

    // Install tab
    readonly TextBlock gamePath = new() { TextWrapping = TextWrapping.Wrap };
    readonly TextBlock gameStatus = new() { TextWrapping = TextWrapping.Wrap };
    readonly CheckBox dlcEnabled = new() { Content = "Include my DLC (experimental)" };
    readonly TextBlock dlcStatus = new() { TextWrapping = TextWrapping.Wrap, Foreground = Muted };
    readonly ProgressBar progress = new() { Minimum = 0, Maximum = 1, Height = 10 };
    readonly TextBlock statusText = new() { TextWrapping = TextWrapping.Wrap };
    readonly Button buildButton = AccentButton("BUILD");
    readonly Button cancelButton = new() { Content = "Cancel", IsVisible = false };
    readonly Button playButton = AccentButton("PLAY", 18);

    // Settings tab
    readonly CheckBox setFullscreen = new() { Content = "Start in fullscreen (borderless)" };
    readonly CheckBox setFullGame = new() { Content = "Full game, not the trial (license_mask = 1)" };
    readonly CheckBox setNetplay = new() { Content = "Online co-op over LAN / virtual LAN" };
    readonly TextBox setName = new() { Watermark = "Your name for other players (max. 15 characters)", MaxLength = 15 };
    readonly TextBox setPeers = new() { Watermark = "Other players' IPs, comma separated (only if the search finds nothing)" };
    readonly TextBlock settingsHint = new() { TextWrapping = TextWrapping.Wrap, Foreground = Muted };
    readonly Button saveSettings = AccentButton("Save settings");
    readonly StackPanel settingsFields = new() { Spacing = 10 };

    // Log tab
    readonly TextBox logBox = new()
    {
        IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.NoWrap,
        FontFamily = new FontFamily("Cascadia Mono,Consolas,monospace"), FontSize = 12,
    };
    readonly ConcurrentQueue<string> pendingLines = new();
    readonly StringBuilder logText = new();
    StreamWriter? logFile;

    public MainWindow()
    {
        Title = "HarmonyRecomp Launcher";
        Width = 920;
        Height = 860;
        MinWidth = 760;
        MinHeight = 600;
        Background = new LinearGradientBrush
        {
            StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
            EndPoint = new RelativePoint(1, 1, RelativeUnit.Relative),
            GradientStops = { new GradientStop(Color.Parse("#120A16"), 0), new GradientStop(Color.Parse("#2A0F1E"), 1) },
        };
        Content = BuildUi();

        dlcEnabled.IsChecked = config.DlcEnabled;
        dlcEnabled.IsCheckedChanged += (_, _) => { config.DlcEnabled = dlcEnabled.IsChecked == true; config.Save(); RefreshDlc(); };
        buildButton.Click += async (_, _) => await BuildAsync();
        cancelButton.Click += (_, _) => cts?.Cancel();
        playButton.Click += (_, _) => Play();
        saveSettings.Click += (_, _) => SaveSettings();
        Closing += (_, _) => cts?.Cancel();

        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
        timer.Tick += (_, _) => Tick();
        timer.Start();

        RefreshGame();
        RefreshDlc();
        RefreshPlay();
        LoadSettings();
        statusText.Text = layout == null
            ? "The HarmonyRecomp source will be downloaded when you press BUILD."
            : File.Exists(layout.GameExe) ? "Built and ready. Press BUILD again after updating your files or the source."
            : "Pick your game, then press BUILD.";
    }

    // ---- UI ---------------------------------------------------------------

    static Button AccentButton(string text, double size = 14) => new()
    {
        Content = text, Background = Crimson, Foreground = Brushes.White, FontWeight = FontWeight.Bold,
        FontSize = size, Padding = new Thickness(26, 10), CornerRadius = new CornerRadius(6),
    };

    static Border Card(string title, params Control[] children)
    {
        var panel = new StackPanel { Spacing = 10 };
        panel.Children.Add(new TextBlock { Text = title, FontSize = 17, FontWeight = FontWeight.SemiBold, Foreground = Gold });
        foreach (var c in children)
            panel.Children.Add(c);
        return new Border
        {
            Background = CardBg, BorderBrush = CardBorder, BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10), Padding = new Thickness(18, 14), Child = panel,
        };
    }

    static TextBlock Note(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = Muted, FontSize = 13 };

    static StackPanel Row(params Control[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        foreach (var c in children)
            row.Children.Add(c);
        return row;
    }

    Control BuildUi()
    {
        var header = new StackPanel
        {
            Margin = new Thickness(28, 22, 28, 8),
            Children =
            {
                new TextBlock { Text = "HARMONY RECOMP", FontSize = 34, FontWeight = FontWeight.Bold, Foreground = Gold, LetterSpacing = 4 },
                new TextBlock { Text = "Castlevania: Harmony of Despair, built natively for PC from your own copy", Foreground = Muted },
            },
        };

        var browseGame = new Button { Content = "Choose game file…" };
        browseGame.Click += async (_, _) => await PickGameAsync();
        var addDlc = new Button { Content = "Add DLC files…" };
        addDlc.Click += async (_, _) => await PickDlcAsync();
        var clearDlc = new Button { Content = "Clear" };
        clearDlc.Click += (_, _) => { config.DlcPackages.Clear(); config.Save(); RefreshDlc(); };

        var install = new ScrollViewer
        {
            Content = new StackPanel
            {
                Spacing = 14, Margin = new Thickness(28, 8),
                Children =
                {
                    Card("1 · Your game",
                        Note("Your own XBLA package: the file without extension in 58410A7A\\000D0000\\ (it starts with LIVE). " +
                             "No game files come with this launcher."),
                        Row(browseGame), gamePath, gameStatus),
                    Card("2 · DLC",
                        Note("Your DLC packages from 58410A7A\\00000002\\. DLC support is experimental: some packages do not " +
                             "recompile yet. If the build fails, turn this off and build again."),
                        dlcEnabled, Row(addDlc, clearDlc), dlcStatus),
                    Card("3 · Build",
                        Note("The first build downloads about 850 MB of build tools (LLVM, CMake, the Windows SDK, ...) into a " +
                             "folder next to the launcher, then takes a few minutes (20 to 40 if the ReXGlue SDK has to be " +
                             "compiled too). Nothing is installed on your system. Later builds only redo what changed."),
                        progress, statusText, Row(buildButton, cancelButton)),
                },
            },
        };

        settingsFields.Children.Add(setFullscreen);
        settingsFields.Children.Add(setFullGame);
        settingsFields.Children.Add(setNetplay);
        settingsFields.Children.Add(setName);
        settingsFields.Children.Add(setPeers);
        settingsFields.Children.Add(Note("For co-op every player needs a different name and the same HarmonyRecomp version. " +
                                         "Over the internet use Radmin VPN or ZeroTier; with Tailscale enter the others' IPs above. " +
                                         "In game: F6 graphics menu, F2 FPS counter."));
        settingsFields.Children.Add(Row(saveSettings));
        var settings = new ScrollViewer
        {
            Content = new StackPanel
            {
                Margin = new Thickness(28, 8), Spacing = 14,
                Children = { Card("Game settings", settingsHint, settingsFields) },
            },
        };

        var openLogs = new Button { Content = "Open game logs" };
        openLogs.Click += (_, _) => OpenFolder(layout?.LogsDir);
        var openGame = new Button { Content = "Open game folder" };
        openGame.Click += (_, _) => OpenFolder(layout?.OutDir);
        var logPanel = new DockPanel { Margin = new Thickness(28, 8) };
        var logButtons = Row(openLogs, openGame);
        logButtons.Margin = new Thickness(0, 8, 0, 0);
        DockPanel.SetDock(logButtons, Dock.Bottom);
        logPanel.Children.Add(logButtons);
        logPanel.Children.Add(logBox);

        var tabs = new TabControl
        {
            Margin = new Thickness(16, 0),
            ItemsSource = new[]
            {
                new TabItem { Header = "Install", Content = install },
                new TabItem { Header = "Settings", Content = settings },
                new TabItem { Header = "Build log", Content = logPanel },
            },
        };
        tabs.SelectionChanged += (_, _) => { if (tabs.SelectedIndex == 1) LoadSettings(); };

        var footer = new DockPanel { Margin = new Thickness(28, 10, 28, 20) };
        DockPanel.SetDock(playButton, Dock.Right);
        footer.Children.Add(playButton);
        footer.Children.Add(new TextBlock
        {
            Text = "Unofficial fan project. Castlevania is a trademark of Konami. Not affiliated with Konami or Microsoft.",
            Foreground = Muted, FontSize = 12, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap,
        });

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto") };
        Grid.SetRow(tabs, 1);
        Grid.SetRow(footer, 2);
        root.Children.Add(header);
        root.Children.Add(tabs);
        root.Children.Add(footer);
        return root;
    }

    // ---- Game and DLC selection --------------------------------------------

    async Task PickGameAsync()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose your Harmony of Despair package (58410A7A\\000D0000\\...)", AllowMultiple = false,
        });
        if (files.Count == 0 || files[0].TryGetLocalPath() is not { } path)
            return;
        config.GamePackage = path;
        config.Save();
        RefreshGame();
    }

    async Task PickDlcAsync()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose your DLC packages (58410A7A\\00000002\\...)", AllowMultiple = true,
        });
        foreach (var file in files)
            if (file.TryGetLocalPath() is { } path && !config.DlcPackages.Contains(path, StringComparer.OrdinalIgnoreCase))
                config.DlcPackages.Add(path);
        config.Save();
        RefreshDlc();
    }

    PackageInfo? ValidGame() =>
        config.GamePackage is { } p && Stfs.TryRead(p) is { IsBaseGame: true } info ? info : null;

    List<PackageInfo> ValidDlc() =>
        config.DlcPackages.Select(Stfs.TryRead).OfType<PackageInfo>().Where(i => i.IsDlc).ToList();

    void RefreshGame()
    {
        gamePath.Text = config.GamePackage ?? "No file chosen yet.";
        if (config.GamePackage == null)
        {
            gameStatus.Text = "";
            return;
        }
        var info = Stfs.TryRead(config.GamePackage);
        (gameStatus.Text, gameStatus.Foreground) = info switch
        {
            null => ("Not an Xbox 360 LIVE/PIRS package, or the file cannot be read.", Bad),
            { IsBaseGame: true } => ($"✔ {(info.DisplayName.Length > 0 ? info.DisplayName : "Castlevania: Harmony of Despair")} (title 58410A7A)", Good),
            { IsDlc: true } => ("This is a DLC package. Add it under DLC and choose the game itself here.", Bad),
            { IsThisGame: false } => ($"This package belongs to another game (title {info.TitleId:X8}).", Bad),
            _ => ($"Unexpected package type {info.ContentType:X8}.", Bad),
        };
    }

    void RefreshDlc()
    {
        var valid = ValidDlc();
        int invalid = config.DlcPackages.Count - valid.Count;
        var text = new StringBuilder(config.DlcPackages.Count == 0 ? "No DLC packages added." : $"{valid.Count} DLC package(s) added");
        if (valid.Count > 0)
            text.Append(": ").Append(string.Join(", ", valid.Select(v => v.DisplayName.Length > 0 ? v.DisplayName : Path.GetFileName(v.Path))));
        if (invalid > 0)
            text.Append($"\n{invalid} file(s) are not Harmony of Despair DLC and will be skipped.");
        if (dlcEnabled.IsChecked != true && valid.Count > 0)
            text.Append("\nNot included: the checkbox above is off.");
        dlcStatus.Text = text.ToString();
        dlcStatus.Foreground = invalid > 0 ? Warn : Muted;
    }

    void RefreshPlay()
    {
        bool built = layout != null && File.Exists(layout.GameExe);
        playButton.IsEnabled = built && cts == null;
        buildButton.IsEnabled = cts == null;
        cancelButton.IsVisible = cts != null;
    }

    // ---- Build --------------------------------------------------------------

    async Task BuildAsync()
    {
        if (ValidGame() == null)
        {
            SetStatus("Choose your game package first (step 1).", Bad);
            return;
        }
        cts = new CancellationTokenSource();
        RefreshPlay();
        logText.Clear();
        logBox.Text = "";
        prepProgress = 0;
        runner = null;
        try
        {
            logFile = new StreamWriter(Path.Combine(Layout.LauncherDir, "launcher.log"), append: false) { AutoFlush = true };
        }
        catch (IOException) { logFile = null; }
        catch (UnauthorizedAccessException) { logFile = null; }

        try
        {
            var ct = cts.Token;
            var prep = new Progress<double>(p => prepProgress = p);
            if (layout == null)
            {
                SetStatus("Downloading the HarmonyRecomp source…", null);
                Log($"Downloading {Downloads.SourceZipUrl}");
                await Downloads.DownloadSourceAsync(Layout.DefaultSource, prep, ct);
                if (!Layout.IsSource(Layout.DefaultSource))
                    throw new InvalidOperationException(
                        "The downloaded source has no toolchain.lock.json yet, so this launcher cannot build it.");
                layout = new Layout(Layout.DefaultSource);
            }
            Log($"Source: {layout.Source}");
            Log($"Tools:  {layout.ToolsDir}");

            SetStatus("Downloading Python…", null);
            await Downloads.EnsurePythonAsync(layout, prep, ct);

            SetStatus("Copying your game files…", null);
            var game = ValidGame()!;
            var dlc = dlcEnabled.IsChecked == true ? ValidDlc() : [];
            await Task.Run(() =>
            {
                SyncFolder(layout.GameDir, [game.Path]);
                SyncFolder(layout.DlcDir, dlc.Select(d => d.Path).ToList());
            }, ct);
            Log($"Game: {game.Path}");
            Log($"DLC:  {(dlc.Count == 0 ? "none" : string.Join(", ", dlc.Select(d => Path.GetFileName(d.Path))))}");

            runner = new BuildRunner(layout, Log);
            SetStatus("Preparing the build tools…", null);
            if (await runner.RunPythonAsync("toolchain.py", ["fetch"], ct) != 0)
                throw new InvalidOperationException("Downloading the build tools failed. See the build log.");
            if (await runner.RunPythonAsync("build.py", ["--toolchain", "portable"], ct) != 0)
                throw new InvalidOperationException(dlc.Count > 0
                    ? "The build failed. DLC support is experimental: turn DLC off and press BUILD again. Details are in the build log."
                    : "The build failed. See the build log for details.");
            SetStatus("✔ Built. Press PLAY!", Good);
            progress.Value = 1;
        }
        catch (OperationCanceledException)
        {
            SetStatus("Cancelled. Press BUILD to continue where it stopped.", Warn);
        }
        catch (Exception e) when (e is InvalidOperationException or IOException or HttpRequestException
                                      or InvalidDataException or UnauthorizedAccessException)
        {
            Log("error: " + e.Message);
            SetStatus(e.Message, Bad);
        }
        finally
        {
            runner = null;
            cts.Dispose();
            cts = null;
            logFile?.Dispose();
            logFile = null;
            RefreshPlay();
            LoadSettings();
        }
    }

    // Makes dir contain exactly these files (by name), copying only what changed.
    // File.Copy keeps the modification time, which build.py uses to notice new DLC.
    static void SyncFolder(string dir, IReadOnlyList<string> files)
    {
        Directory.CreateDirectory(dir);
        var names = files.Select(Path.GetFileName).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var existing in Directory.GetFiles(dir))
            if (!names.Contains(Path.GetFileName(existing)))
                File.Delete(existing);
        foreach (var src in files)
        {
            var dest = Path.Combine(dir, Path.GetFileName(src));
            if (string.Equals(Path.GetFullPath(src), Path.GetFullPath(dest), StringComparison.OrdinalIgnoreCase))
                continue;
            var s = new FileInfo(src);
            var d = new FileInfo(dest);
            if (d.Exists && d.Length == s.Length && d.LastWriteTimeUtc == s.LastWriteTimeUtc)
                continue;
            File.Copy(src, dest, overwrite: true);
        }
    }

    void SetStatus(string text, IBrush? color)
    {
        statusText.Text = text;
        statusText.Foreground = color ?? Brushes.White;
    }

    // Called from the build's output threads; the timer moves lines to the UI.
    void Log(string line)
    {
        pendingLines.Enqueue(line);
        try { logFile?.WriteLine(line); } catch (ObjectDisposedException) { }
    }

    void Tick()
    {
        if (!pendingLines.IsEmpty)
        {
            while (pendingLines.TryDequeue(out var line))
                logText.AppendLine(line);
            const int keep = 400_000;  // characters; the full log is in launcher.log
            if (logText.Length > keep)
                logText.Remove(0, logText.Length - keep);
            logBox.Text = logText.ToString();
            logBox.CaretIndex = logBox.Text.Length;
        }
        if (cts == null)
            return;
        if (runner != null)
        {
            progress.Value = runner.Progress;
            if (runner.Status.Length > 0)
                SetStatus(runner.Status, null);
        }
        else
        {
            progress.Value = 0.02 * prepProgress;
        }
    }

    // ---- Play and settings --------------------------------------------------

    void Play()
    {
        if (layout == null || !File.Exists(layout.GameExe))
            return;
        Process.Start(new ProcessStartInfo(layout.GameExe) { WorkingDirectory = layout.OutDir, UseShellExecute = false });
    }

    void LoadSettings()
    {
        bool available = layout != null && File.Exists(layout.RuntimeConfig);
        settingsFields.IsEnabled = available;
        settingsHint.Text = available
            ? $"Saved to {layout!.RuntimeConfig}. Changes apply the next time the game starts."
            : "Build the game first: the settings file is created next to the game.";
        if (!available)
            return;
        var s = RuntimeSettings.Load(layout!.RuntimeConfig);
        setFullscreen.IsChecked = s.GetBool("fullscreen", false);
        setFullGame.IsChecked = s.GetBool("license_mask", false);
        setNetplay.IsChecked = s.GetBool("netplay_enabled", false);
        setName.Text = s.GetString("netplay_name");
        setPeers.Text = s.GetString("netplay_peers");
    }

    void SaveSettings()
    {
        if (layout == null || !File.Exists(layout.RuntimeConfig))
            return;
        var s = RuntimeSettings.Load(layout.RuntimeConfig);
        s.SetBool("fullscreen", setFullscreen.IsChecked == true);
        s.SetInt("license_mask", setFullGame.IsChecked == true ? 1 : 0);
        s.SetBool("netplay_enabled", setNetplay.IsChecked == true);
        s.SetString("netplay_name", (setName.Text ?? "").Trim());
        if ((setPeers.Text ?? "").Trim() is { Length: > 0 } peers)
            s.SetString("netplay_peers", peers);
        s.Save();
        settingsHint.Text = $"✔ Saved to {layout.RuntimeConfig}. Changes apply the next time the game starts.";
    }

    static void OpenFolder(string? dir)
    {
        if (dir != null && Directory.Exists(dir))
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = false });
    }
}
