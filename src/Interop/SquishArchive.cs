// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A resource-safe managed wrapper over a squish_archive* reader handle, plus
// the static archive-creation entry point. The wrapper reads the whole entry
// index eagerly on open (cheap: proportional to member count, no decompression)
// so the UI has a stable, handle-independent snapshot to bind against.

using System.IO;
using System.Runtime.InteropServices;

namespace WinSquish.Interop;

/// <summary>Archive-wide header, from <c>squish_archive_info</c>.</summary>
public readonly record struct ArchiveInfo(
    uint Version, uint Flags, ulong EntryCount, ulong TotalSize, ulong ChunkSize);

/// <summary>One member of an archive (file or directory), copied out of the handle.</summary>
public sealed class SquishEntry
{
    /// <summary>Relative, '/'-separated, UTF-8 path. "" for a blob's lone member.</summary>
    public required string Path { get; init; }
    public required ulong Size { get; init; }
    public required ulong StoredSize { get; init; }
    public required uint Mode { get; init; }
    public required bool IsDir { get; init; }
    public required ulong Index { get; init; }

    /// <summary>Final path segment (the display name).</summary>
    public string Name
    {
        get
        {
            var p = Path.TrimEnd('/');
            int slash = p.LastIndexOf('/');
            return slash < 0 ? p : p[(slash + 1)..];
        }
    }

    /// <summary>Compressed / uncompressed, or 0 for empty/dir entries.</summary>
    public double Ratio => Size == 0 ? 0 : (double)StoredSize / Size;
}

/// <summary>
/// A read-only view over a SQUISH archive. Open with <see cref="Open"/>, browse
/// <see cref="Entries"/>, and extract by index/path. Not thread-safe: a handle
/// serializes its own calls, so run extractions on a single background thread.
/// </summary>
public sealed class SquishArchive : IDisposable
{
    private IntPtr _handle;
    private readonly object _gate = new();

    public string ArchivePath { get; }
    public ArchiveInfo Info { get; }
    public IReadOnlyList<SquishEntry> Entries { get; }

    private SquishArchive(IntPtr handle, string archivePath)
    {
        _handle = handle;
        ArchivePath = archivePath;

        SquishException.Check(
            SquishNative.squish_archive_info_get(handle, out var infoN), "read archive info");
        Info = new ArchiveInfo(
            infoN.Version, infoN.Flags, infoN.EntryCount, infoN.TotalSize, infoN.ChunkSize);

        ulong count = SquishNative.squish_archive_count(handle);
        var entries = new List<SquishEntry>((int)Math.Min(count, int.MaxValue));
        for (ulong i = 0; i < count; i++)
        {
            SquishException.Check(
                SquishNative.squish_archive_stat(handle, i, out var e), $"stat entry {i}");
            entries.Add(new SquishEntry
            {
                Path = Marshal.PtrToStringUTF8(e.Path) ?? string.Empty,
                Size = e.Size,
                StoredSize = e.StoredSize,
                Mode = e.Mode,
                IsDir = e.IsDir != 0,
                Index = i,
            });
        }
        Entries = entries;
    }

    /// <summary>Open an archive file for reading. Validates header + index only.</summary>
    public static SquishArchive Open(string path)
    {
        SquishException.Check(SquishNative.squish_archive_open(path, out var handle), $"open '{path}'");
        try
        {
            return new SquishArchive(handle, path);
        }
        catch
        {
            SquishNative.squish_archive_close(handle);
            throw;
        }
    }

    /// <summary>Extract one member fully into managed memory.</summary>
    public byte[] Extract(SquishEntry entry)
    {
        if (entry.IsDir)
            throw new InvalidOperationException("Cannot extract a directory to a buffer.");

        lock (_gate)
        {
            EnsureOpen();
            SquishException.Check(
                SquishNative.squish_archive_extract(_handle, entry.Index, out var buf, out var len),
                $"extract '{entry.Path}'");
            try
            {
                var managed = new byte[(long)len];
                if ((long)len > 0)
                    Marshal.Copy(buf, managed, 0, checked((int)len));
                return managed;
            }
            finally
            {
                SquishNative.squish_free(buf);
            }
        }
    }

    /// <summary>Extract one file member straight to disk (parent dirs must exist).</summary>
    public void ExtractToFile(SquishEntry entry, string dstPath)
    {
        lock (_gate)
        {
            EnsureOpen();
            SquishException.Check(
                SquishNative.squish_archive_extract_to_file(_handle, entry.Path, dstPath),
                $"extract '{entry.Path}' to file");
        }
    }

    /// <summary>
    /// Recreate every member at or beneath <paramref name="prefix"/> under
    /// <paramref name="dstRoot"/>. A null/empty prefix extracts the whole archive.
    /// </summary>
    public void ExtractSubtree(string? prefix, string dstRoot, Action<ulong, ulong>? progress = null)
    {
        SquishProgressFn? cb = progress is null
            ? null
            : (processed, total, _) => progress(processed, total);

        lock (_gate)
        {
            EnsureOpen();
            SquishException.Check(
                SquishNative.squish_archive_extract_subtree(_handle, prefix, dstRoot, cb, IntPtr.Zero),
                "extract subtree");
        }
        GC.KeepAlive(cb);
    }

    /// <summary>
    /// Build a seekable archive from a directory tree. <paramref name="threads"/>
    /// 0 = all cores, 1 = serial; <paramref name="chunkSize"/> 0 = default 16 MiB.
    /// </summary>
    public static void Create(
        string dirPath, string archivePath, int threads = 1, nuint chunkSize = 0,
        Action<ulong, ulong>? progress = null)
    {
        SquishProgressFn? cb = progress is null
            ? null
            : (processed, total, _) => progress(processed, total);

        SquishException.Check(
            SquishNative.squish_archive_create(dirPath, archivePath, threads, chunkSize, cb, IntPtr.Zero),
            "create archive");
        GC.KeepAlive(cb);
    }

    /// <summary>Compress a single file into a one-member archive.</summary>
    public static void CompressFile(
        string srcPath, string archivePath, int threads = 0, nuint chunkSize = 0,
        Action<ulong, ulong>? progress = null)
    {
        SquishProgressFn? cb = progress is null
            ? null
            : (processed, total, _) => progress(processed, total);

        SquishException.Check(
            SquishNative.squish_compress_file(srcPath, archivePath, threads, chunkSize, cb, IntPtr.Zero),
            "compress file");
        GC.KeepAlive(cb);
    }

    /// <summary>
    /// Add (merge) a file or directory tree into an existing archive under
    /// <paramref name="arcPath"/> ('/'-separated). Existing members are copied
    /// verbatim — only the added files are compressed. Colliding files are
    /// overwritten unless <paramref name="keepExisting"/>. The archive at
    /// <paramref name="archivePath"/> is rewritten in place (atomic swap), so no
    /// reader handle to it may be open. <paramref name="threads"/> 0 = all cores.
    /// </summary>
    public static void Add(
        string archivePath, string srcPath, string arcPath, bool keepExisting = false,
        int threads = 0, Action<ulong, ulong>? progress = null)
    {
        SquishProgressFn? cb = progress is null
            ? null
            : (processed, total, _) => progress(processed, total);

        uint flags = keepExisting ? 1u : 0u;   // SQUISH_ADD_KEEP_EXISTING
        SquishException.Check(
            SquishNative.squish_archive_add(archivePath, srcPath, arcPath, threads, flags, cb, IntPtr.Zero),
            $"add '{arcPath}'");
        GC.KeepAlive(cb);
    }

    /// <summary>
    /// Remove members (each an exact path or a subtree root) from an existing
    /// archive. Survivors are copied verbatim; nothing is recompressed. Rewrites
    /// the archive in place, so no reader handle to it may be open.
    /// </summary>
    public static void Remove(
        string archivePath, IReadOnlyList<string> paths, Action<ulong, ulong>? progress = null)
    {
        if (paths.Count == 0) return;

        SquishProgressFn? cb = progress is null
            ? null
            : (processed, total, _) => progress(processed, total);

        // Marshal the paths as a native array of UTF-8 char* and pin it.
        var ptrs = new IntPtr[paths.Count];
        var pin = default(GCHandle);
        try
        {
            for (int i = 0; i < paths.Count; i++)
                ptrs[i] = Marshal.StringToCoTaskMemUTF8(paths[i]);
            pin = GCHandle.Alloc(ptrs, GCHandleType.Pinned);
            SquishException.Check(
                SquishNative.squish_archive_remove(
                    archivePath, pin.AddrOfPinnedObject(), (nuint)paths.Count, cb, IntPtr.Zero),
                "remove from archive");
        }
        finally
        {
            if (pin.IsAllocated) pin.Free();
            foreach (var p in ptrs)
                if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p);   // frees StringToCoTaskMemUTF8
        }
        GC.KeepAlive(cb);
    }

    /// <summary>Cheap sniff: are the first bytes of this file a SQUISH archive?</summary>
    public static bool Probe(string path)
    {
        try
        {
            using var fs = File.OpenRead(path);
            Span<byte> head = stackalloc byte[64];
            int n = fs.Read(head);
            var buf = head[..n].ToArray();
            return SquishNative.squish_archive_probe(buf, (nuint)buf.Length) != 0;
        }
        catch
        {
            return false;
        }
    }

    private void EnsureOpen()
    {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(SquishArchive));
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_handle != IntPtr.Zero)
            {
                SquishNative.squish_archive_close(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
