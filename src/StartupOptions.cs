// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parses the command line into one startup action. The GUI modes (open a file,
// extract, compress) drive the window; the registration modes run headless and
// exit. Invoked from App.OnStartup before any window exists.

namespace WinSquish;

public enum StartupMode
{
    Open,          // (default) optionally open the archive in Path
    ExtractHere,   // extract Path's archive into its own folder
    ExtractTo,     // extract Path's archive into a subfolder named after it
    Compress,      // compress the file/folder Path into a .sqsh next to it
    Register,      // write shell integration, then exit
    Unregister,    // remove shell integration, then exit
}

public sealed class StartupOptions
{
    public StartupMode Mode { get; private init; } = StartupMode.Open;
    public string? Path { get; private init; }
    public bool AllUsers { get; private init; }
    public bool Quiet { get; private init; }

    public bool IsHeadless => Mode is StartupMode.Register or StartupMode.Unregister;

    public static StartupOptions Parse(string[] args)
    {
        var mode = StartupMode.Open;
        string? path = null;
        bool allUsers = false, quiet = false;

        foreach (var a in args)
        {
            switch (a.ToLowerInvariant())
            {
                case "--register": mode = StartupMode.Register; break;
                case "--unregister": mode = StartupMode.Unregister; break;
                case "--extract-here": mode = StartupMode.ExtractHere; break;
                case "--extract-to": mode = StartupMode.ExtractTo; break;
                case "--compress": mode = StartupMode.Compress; break;
                case "--allusers": allUsers = true; break;
                case "-q" or "--quiet": quiet = true; break;
                default:
                    if (!a.StartsWith('-')) path = a; // the file/folder operand
                    break;
            }
        }

        return new StartupOptions { Mode = mode, Path = path, AllUsers = allUsers, Quiet = quiet };
    }
}
