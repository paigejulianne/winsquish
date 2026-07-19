// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Raw P/Invoke surface for squish.dll. Everything here is 1:1 with squish.h;
// the friendly, resource-safe layer lives in SquishArchive.cs. All exports are
// undecorated C ("SQSH" x64 build), so the default entry-point names match and
// the calling convention is Cdecl. Strings crossing the boundary are UTF-8,
// matching the "'/'-separated, UTF-8" paths the format uses.

using System.Runtime.InteropServices;

namespace WinSquish.Interop;

/// <summary>Status codes returned by every libsquish API function.</summary>
internal enum SquishStatus
{
    Ok = 0,
    Param = -1,
    NoMem = -2,
    Format = -3,
    DstSize = -4,
    TooBig = -5,
    Io = -6,
    Checksum = -7,
}

/// <summary>Mirror of <c>squish_archive_info</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct ArchiveInfoNative
{
    public uint Version;
    public uint Flags;
    public ulong EntryCount;
    public ulong TotalSize;
    public ulong ChunkSize;
}

/// <summary>
/// Mirror of <c>squish_archive_entry</c>. <see cref="Path"/> points into the
/// owning handle and is only valid until the archive is closed, so callers copy
/// it out immediately (see <see cref="SquishArchive"/>).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct ArchiveEntryNative
{
    public IntPtr Path;        // const char*  (UTF-8, owned by the handle)
    public ulong Size;
    public ulong StoredSize;
    public uint Mode;
    public int IsDir;
}

/// <summary>
/// Progress callback: <paramref name="processed"/> of <paramref name="total"/>
/// original bytes handled. Keep the managed delegate rooted for the whole call.
/// </summary>
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate void SquishProgressFn(ulong processed, ulong total, IntPtr user);

internal static class SquishNative
{
    private const string Dll = "squish.dll";
    private const CallingConvention Cc = CallingConvention.Cdecl;

    // --- introspection ---------------------------------------------------
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern IntPtr squish_version();

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern IntPtr squish_strerror(int code);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_threads();

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern void squish_free(IntPtr p);

    // --- sniffing --------------------------------------------------------
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_probe(byte[] data, nuint len);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_content_size(byte[] src, nuint srcLen, out ulong outSize);

    // --- open / close ----------------------------------------------------
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_open(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, out IntPtr outHandle);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern void squish_archive_close(IntPtr a);

    // --- listing ---------------------------------------------------------
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_info_get(IntPtr a, out ArchiveInfoNative outInfo);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern ulong squish_archive_count(IntPtr a);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_stat(IntPtr a, ulong index, out ArchiveEntryNative outEntry);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_find(
        IntPtr a, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, out ulong indexOut);

    // --- extraction ------------------------------------------------------
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_extract(IntPtr a, ulong index, out IntPtr outBuf, out nuint outLen);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_extract_path(
        IntPtr a, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, out IntPtr outBuf, out nuint outLen);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_extract_to_file(
        IntPtr a,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dstPath);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_extract_subtree(
        IntPtr a,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? prefix,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dstRoot,
        SquishProgressFn? cb, IntPtr user);

    // --- creation --------------------------------------------------------
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_compress_file(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string srcPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dstPath,
        int nthreads, nuint chunkSize,
        SquishProgressFn? cb, IntPtr user);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int squish_archive_create(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dirPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string archivePath,
        int nthreads, nuint chunkSize,
        SquishProgressFn? cb, IntPtr user);

    // --- helpers ---------------------------------------------------------
    public static string Version() => Marshal.PtrToStringUTF8(squish_version()) ?? "?";

    public static string StrError(SquishStatus code) =>
        Marshal.PtrToStringUTF8(squish_strerror((int)code)) ?? $"error {(int)code}";
}
