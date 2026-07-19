// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns the archive's flat, '/'-separated entry list into a browsable tree of
// folders and files. Folders are synthesized where paths imply them, so the
// tree is well-formed even if the archive stores no explicit directory members.

using System.ComponentModel;
using WinSquish.Interop;

namespace WinSquish.Models;

public sealed class ArchiveNode : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    private bool _isExpanded;
    /// <summary>Tree expansion state, two-way bound to the TreeViewItem.</summary>
    public bool IsExpanded
    {
        get => _isExpanded;
        set { if (_isExpanded != value) { _isExpanded = value; Notify(nameof(IsExpanded)); } }
    }

    private bool _isSelected;
    /// <summary>Tree selection state, two-way bound to the TreeViewItem.</summary>
    public bool IsSelected
    {
        get => _isSelected;
        set { if (_isSelected != value) { _isSelected = value; Notify(nameof(IsSelected)); } }
    }

    private void Notify(string name) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    /// <summary>Expand every ancestor so this node is realized/visible in the tree.</summary>
    public void ExpandAncestors()
    {
        for (var n = Parent; n != null; n = n.Parent) n.IsExpanded = true;
    }

    /// <summary>Display name (final path segment). The root uses the archive filename.</summary>
    public string Name { get; }

    /// <summary>Full '/'-separated path within the archive ("" for the root).</summary>
    public string FullPath { get; }

    public bool IsDirectory { get; }

    /// <summary>The backing member, for files (null for folders and the root).</summary>
    public SquishEntry? Entry { get; }

    public ArchiveNode? Parent { get; private set; }

    private readonly List<ArchiveNode> _children = new();
    public IReadOnlyList<ArchiveNode> Children => _children;

    /// <summary>Uncompressed size: the file's size, or the sum of a folder's files.</summary>
    public ulong Size { get; private set; }

    /// <summary>Compressed size: the file's stored size, or the folder's total.</summary>
    public ulong StoredSize { get; private set; }

    /// <summary>Number of file (non-directory) descendants.</summary>
    public int FileCount { get; private set; }

    private ArchiveNode(string name, string fullPath, bool isDir, SquishEntry? entry)
    {
        Name = name;
        FullPath = fullPath;
        IsDirectory = isDir;
        Entry = entry;
    }

    public IEnumerable<ArchiveNode> Folders => _children.Where(c => c.IsDirectory);
    public IEnumerable<ArchiveNode> Files => _children.Where(c => !c.IsDirectory);

    /// <summary>Compression ratio (stored/size) for a file or aggregated folder.</summary>
    public double Ratio => Size == 0 ? 0 : (double)StoredSize / Size;

    /// <summary>Build a tree from an opened archive. Returns the root node.</summary>
    public static ArchiveNode BuildTree(SquishArchive archive, string rootName)
    {
        var root = new ArchiveNode(rootName, "", isDir: true, entry: null);
        var folders = new Dictionary<string, ArchiveNode>(StringComparer.Ordinal) { [""] = root };

        foreach (var entry in archive.Entries)
        {
            string path = entry.Path.Trim('/');
            if (path.Length == 0) // unnamed blob member — synthesize a name
                path = string.IsNullOrEmpty(rootName) ? "(data)" : rootName;

            var segments = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
            var parent = root;

            for (int i = 0; i < segments.Length; i++)
            {
                bool isLast = i == segments.Length - 1;
                string prefix = string.Join('/', segments[..(i + 1)]);

                if (isLast && !entry.IsDir)
                {
                    var file = new ArchiveNode(segments[i], prefix, isDir: false, entry: entry);
                    parent.AddChild(file);
                    file.Size = entry.Size;
                    file.StoredSize = entry.StoredSize;
                    file.FileCount = 1;
                    break;
                }

                // A directory level — reuse or create the folder node.
                if (!folders.TryGetValue(prefix, out var folder))
                {
                    folder = new ArchiveNode(segments[i], prefix, isDir: true,
                        entry: isLast ? entry : null);
                    parent.AddChild(folder);
                    folders[prefix] = folder;
                }
                parent = folder;
            }
        }

        root.Accumulate();
        root.SortChildren();
        return root;
    }

    private void AddChild(ArchiveNode child)
    {
        child.Parent = this;
        _children.Add(child);
    }

    /// <summary>Roll file sizes/counts up into every ancestor folder.</summary>
    private void Accumulate()
    {
        // Files already carry their own size/count (set at creation); only
        // folders aggregate, summing every child regardless of kind.
        if (!IsDirectory) return;
        foreach (var child in _children)
        {
            child.Accumulate();
            Size += child.Size;
            StoredSize += child.StoredSize;
            FileCount += child.FileCount;
        }
    }

    private void SortChildren()
    {
        // Folders first, then files, each alphabetical (case-insensitive).
        _children.Sort((a, b) =>
        {
            if (a.IsDirectory != b.IsDirectory) return a.IsDirectory ? -1 : 1;
            return string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
        });
        foreach (var child in _children)
            child.SortChildren();
    }
}
