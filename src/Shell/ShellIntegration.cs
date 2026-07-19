// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self-registration of the Windows shell integration: the .sqsh/.sq file type,
// its Explorer context-menu verbs (Open / Extract Here / Extract to folder), and
// a "Compress to SQUISH archive" verb on folders. The app is the single source
// of truth — the installer just runs "WinSquish.exe --register [--allusers]", so
// the same keys are written whether installed or associated ad-hoc. --allusers
// targets HKLM (needs elevation); otherwise HKCU\Software\Classes, per-user.

using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace WinSquish.Shell;

public static class ShellIntegration
{
    private const string ProgId = "WinSquish.Archive";
    private const string ProgIdLabel = "SQUISH Archive";
    private const string DirVerb = "WinSquish.Compress";
    private static readonly string[] Extensions = { ".sqsh", ".sq" };

    /// <summary>Full path to the running WinSquish executable.</summary>
    public static string ExePath =>
        Environment.ProcessPath ?? Process.GetCurrentProcess().MainModule!.FileName!;

    public static bool IsElevated
    {
        get
        {
            using var id = System.Security.Principal.WindowsIdentity.GetCurrent();
            var p = new System.Security.Principal.WindowsPrincipal(id);
            return p.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
    }

    /// <summary>Write the file-type, verbs and folder verb into HKLM or HKCU.</summary>
    public static void Register(bool allUsers)
    {
        string exe = ExePath;
        string icon = $"\"{exe}\",0";
        using var classes = OpenClasses(allUsers);

        // The archive ProgId + its icon + open/extract verbs.
        using (var prog = classes.CreateSubKey(ProgId))
        {
            prog.SetValue("", ProgIdLabel);
            using (var di = prog.CreateSubKey("DefaultIcon")) di.SetValue("", icon);
            using (var shell = prog.CreateSubKey("shell"))
            {
                shell.SetValue("", "open");
                Verb(shell, "open", "&Open with WinSquish", $"\"{exe}\" \"%1\"", icon);
                Verb(shell, "extract_here", "Extract &Here", $"\"{exe}\" --extract-here \"%1\"", icon);
                Verb(shell, "extract_to", "Extract to &folder\\", $"\"{exe}\" --extract-to \"%1\"", icon);
            }
        }

        // Point each extension at the ProgId (recording the prior value so we can
        // restore it on unregister rather than clobbering someone else's type).
        foreach (var ext in Extensions)
            using (var k = classes.CreateSubKey(ext))
            {
                if (k.GetValue("") is string prev && prev.Length > 0 && prev != ProgId)
                    k.SetValue("WinSquish.Backup", prev);
                k.SetValue("", ProgId);
            }

        // A "Compress to SQUISH archive" verb on any folder.
        using (var dv = classes.CreateSubKey($@"Directory\shell\{DirVerb}"))
        {
            dv.SetValue("", "Compress to SQUISH archive");
            dv.SetValue("Icon", icon);
            using var cmd = dv.CreateSubKey("command");
            cmd.SetValue("", $"\"{exe}\" --compress \"%1\"");
        }

        NotifyShell();
    }

    /// <summary>Remove everything <see cref="Register"/> wrote, in the same scope.</summary>
    public static void Unregister(bool allUsers)
    {
        using var classes = OpenClasses(allUsers);

        foreach (var ext in Extensions)
            using (var k = classes.OpenSubKey(ext, writable: true))
            {
                if (k is null) continue;
                if (k.GetValue("") as string != ProgId) continue; // not ours; leave it
                if (k.GetValue("WinSquish.Backup") is string prev)
                {
                    k.SetValue("", prev);
                    k.DeleteValue("WinSquish.Backup", throwOnMissingValue: false);
                }
                else
                {
                    k.SetValue("", "");
                }
            }

        classes.DeleteSubKeyTree(ProgId, throwOnMissingSubKey: false);
        classes.DeleteSubKeyTree($@"Directory\shell\{DirVerb}", throwOnMissingSubKey: false);
        NotifyShell();
    }

    /// <summary>True if the extensions currently resolve to our ProgId in either hive.</summary>
    public static bool IsRegistered()
    {
        foreach (var hive in new[] { Registry.CurrentUser, Registry.LocalMachine })
            using (var k = hive.OpenSubKey($@"Software\Classes\{Extensions[0]}"))
                if (k?.GetValue("") as string == ProgId) return true;
        return false;
    }

    private static RegistryKey OpenClasses(bool allUsers)
    {
        var hive = allUsers ? Registry.LocalMachine : Registry.CurrentUser;
        return hive.CreateSubKey(@"Software\Classes")
               ?? throw new InvalidOperationException("Cannot open Software\\Classes for writing.");
    }

    private static void Verb(RegistryKey shell, string name, string label, string command, string icon)
    {
        using var v = shell.CreateSubKey(name);
        v.SetValue("", label);
        v.SetValue("Icon", icon);
        using var c = v.CreateSubKey("command");
        c.SetValue("", command);
    }

    // Tell Explorer the associations changed so icons/verbs refresh without a logoff.
    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

    private static void NotifyShell() =>
        SHChangeNotify(0x08000000 /*SHCNE_ASSOCCHANGED*/, 0 /*SHCNF_IDLIST*/, IntPtr.Zero, IntPtr.Zero);
}
