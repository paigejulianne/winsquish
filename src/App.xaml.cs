// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later

using System.Windows;
using WinSquish.Shell;

namespace WinSquish;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(args.Exception.Message, "WinSquish — unexpected error",
                MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };

        base.OnStartup(e);

        var opts = StartupOptions.Parse(e.Args);

        // Headless registration modes: do the work and exit, no window.
        if (opts.IsHeadless)
        {
            int code = RunRegistration(opts);
            Shutdown(code);
            return;
        }

        // Everything else drives the main window (which runs the action on load).
        ShutdownMode = ShutdownMode.OnMainWindowClose;
        var win = new Views.MainWindow { Startup = opts };
        MainWindow = win;
        win.Show();
    }

    private int RunRegistration(StartupOptions opts)
    {
        try
        {
            if (opts.Mode == StartupMode.Register)
                ShellIntegration.Register(opts.AllUsers);
            else
                ShellIntegration.Unregister(opts.AllUsers);

            if (!opts.Quiet)
            {
                string what = opts.Mode == StartupMode.Register ? "registered" : "removed";
                string scope = opts.AllUsers ? "all users" : "the current user";
                MessageBox.Show($"WinSquish shell integration {what} for {scope}.",
                    "WinSquish", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            return 0;
        }
        catch (UnauthorizedAccessException)
        {
            if (!opts.Quiet)
                MessageBox.Show(
                    "Registering for all users needs administrator rights.\n\n" +
                    "Run WinSquish elevated, or omit --allusers to register for the current user only.",
                    "WinSquish", MessageBoxButton.OK, MessageBoxImage.Warning);
            return 5; // ERROR_ACCESS_DENIED
        }
        catch (Exception ex)
        {
            if (!opts.Quiet)
                MessageBox.Show(ex.Message, "WinSquish — registration failed",
                    MessageBoxButton.OK, MessageBoxImage.Error);
            return 1;
        }
    }
}
