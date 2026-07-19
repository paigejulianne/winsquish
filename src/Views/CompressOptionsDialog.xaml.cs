// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small modal that lets the user tune a compression run — block (chunk) size
// and thread count — before it starts. Both map onto SquishArchive.Create's
// parameters: block size in bytes (0 = libsquish default), threads (0 = all
// cores, 1 = serial). Blank/zero inputs mean "use the default".

using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Input;

namespace WinSquish.Views;

/// <summary>Result of the compression-options prompt, ready for SquishArchive.Create.</summary>
public readonly record struct CompressOptions(int Threads, nuint ChunkSize);

public partial class CompressOptionsDialog : Window
{
    private const nuint MiB = 1024 * 1024;

    private CompressOptionsDialog(string sourceLabel, int coresAvailable)
    {
        InitializeComponent();
        SourceLine.Text = $"Compressing {sourceLabel}";
        ThreadsHint.Text = coresAvailable > 1
            ? $"0 = all {coresAvailable} cores (fastest), 1 = single-threaded. Default is all cores."
            : "0 = all cores, 1 = single-threaded. Default is all cores.";
    }

    /// <summary>The options the user chose, valid only after ShowDialog returns true.</summary>
    public CompressOptions Options { get; private set; }

    /// <summary>
    /// Prompt for block size and thread count. Returns null if the user cancels,
    /// otherwise the chosen options (defaults substituted for blank/zero fields).
    /// </summary>
    public static CompressOptions? Ask(Window owner, string sourceLabel, int coresAvailable)
    {
        var dlg = new CompressOptionsDialog(sourceLabel, coresAvailable) { Owner = owner };
        return dlg.ShowDialog() == true ? dlg.Options : null;
    }

    private void OnDigitsOnly(object sender, TextCompositionEventArgs e) =>
        e.Handled = !Regex.IsMatch(e.Text, "^[0-9]+$");

    private void OnCompress(object sender, RoutedEventArgs e)
    {
        // Blank means "default": 0 MiB block and 0 threads both defer to libsquish.
        nuint mib = ParseOrZero(BlockSizeBox.Text);
        int threads = (int)Math.Min(ParseOrZero(ThreadsBox.Text), (nuint)int.MaxValue);

        Options = new CompressOptions(threads, mib == 0 ? 0 : checked(mib * MiB));
        DialogResult = true;
    }

    private void OnCancel(object sender, RoutedEventArgs e) => DialogResult = false;

    private static nuint ParseOrZero(string? s) =>
        nuint.TryParse(s?.Trim(), out var n) ? n : 0;
}
