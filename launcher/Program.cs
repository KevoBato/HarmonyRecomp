using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;

namespace HarmonyLauncher;

public static class Program
{
    // Optional: --source <folder> uses an existing HarmonyRecomp checkout
    // instead of looking next to the launcher (handy for development).
    public static string? SourceOverride { get; private set; }

    [STAThread]
    public static void Main(string[] args)
    {
        for (int i = 0; i + 1 < args.Length; i++)
            if (args[i] == "--source")
                SourceOverride = Path.GetFullPath(args[i + 1]);

        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace()
            .StartWithClassicDesktopLifetime(args);
    }
}

public sealed class App : Application
{
    public override void Initialize()
    {
        Styles.Add(new FluentTheme());
        RequestedThemeVariant = ThemeVariant.Dark;
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow();
        base.OnFrameworkInitializationCompleted();
    }
}
