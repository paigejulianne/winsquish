// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later

using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Data;
using WinSquish.Models;

namespace WinSquish.Views;

public static class Format
{
    private static readonly string[] Units = { "B", "KB", "MB", "GB", "TB", "PB" };

    /// <summary>Human-readable byte count, e.g. "1.4 MB".</summary>
    public static string Bytes(ulong n)
    {
        double v = n;
        int u = 0;
        while (v >= 1024 && u < Units.Length - 1) { v /= 1024; u++; }
        return u == 0 ? $"{n} {Units[u]}" : $"{v:0.##} {Units[u]}";
    }

    /// <summary>Compression as a saved-percentage, e.g. "72%" means 72% smaller.</summary>
    public static string Saved(double ratio) =>
        ratio <= 0 || ratio >= 1 ? "—" : $"{(1 - ratio) * 100:0}%";
}

/// <summary>ulong byte count -> "1.4 MB".</summary>
public sealed class BytesConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        value is ulong n ? Format.Bytes(n) : "";
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => Binding.DoNothing;
}

/// <summary>Ratio double -> "72%" saved.</summary>
public sealed class RatioConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c) =>
        value is double d ? Format.Saved(d) : "";
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => Binding.DoNothing;
}

/// <summary>ArchiveNode -> a Segoe Fluent glyph for its kind.</summary>
public sealed class NodeGlyphConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        if (value is not ArchiveNode n) return ""; // page
        if (n.IsDirectory) return "";              // folder
        return ExtGlyph(Path.GetExtension(n.Name));
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => Binding.DoNothing;

    private static string ExtGlyph(string ext) => ext.ToLowerInvariant() switch
    {
        ".png" or ".jpg" or ".jpeg" or ".gif" or ".bmp" or ".webp" or ".ico" or ".svg" => "",
        ".txt" or ".md" or ".log" or ".rtf" => "",
        ".c" or ".h" or ".cpp" or ".cs" or ".py" or ".js" or ".ts" or ".java" or ".go"
            or ".rs" or ".rb" or ".xml" or ".json" or ".yml" or ".yaml" or ".html" or ".css" => "",
        ".zip" or ".sqsh" or ".sq" or ".gz" or ".xz" or ".7z" or ".rar" or ".tar" => "",
        ".pdf" => "",
        ".exe" or ".dll" or ".bin" or ".o" or ".obj" or ".a" or ".lib" => "",
        ".mp3" or ".wav" or ".flac" or ".ogg" or ".m4a" => "",
        ".mp4" or ".mkv" or ".mov" or ".avi" or ".webm" => "",
        _ => "",
    };
}

/// <summary>ArchiveNode -> a short type description for the details column.</summary>
public sealed class NodeTypeConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        if (value is not ArchiveNode n) return "";
        if (n.IsDirectory) return "Folder";
        var ext = Path.GetExtension(n.Name);
        return string.IsNullOrEmpty(ext) ? "File" : ext.TrimStart('.').ToUpperInvariant() + " file";
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => Binding.DoNothing;
}

/// <summary>true -> Visible, false -> Collapsed.</summary>
public sealed class BoolToVisibilityConverter : IValueConverter
{
    public bool Invert { get; set; }
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        bool b = value is true;
        if (Invert) b = !b;
        return b ? Visibility.Visible : Visibility.Collapsed;
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => Binding.DoNothing;
}

/// <summary>Folder-only size: blank for empty aggregates so the column stays tidy.</summary>
public sealed class NodeSizeConverter : IValueConverter
{
    public object Convert(object? value, Type t, object? p, CultureInfo c)
    {
        if (value is not ArchiveNode n) return "";
        if (n.IsDirectory && n.FileCount == 0) return "";
        return Format.Bytes(n.Size);
    }
    public object ConvertBack(object? v, Type t, object? p, CultureInfo c) => Binding.DoNothing;
}
