// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The application's single view model: owns the open archive, drives folder
// navigation (tree + breadcrumb + details list), and runs open/create/extract
// operations off the UI thread while surfacing progress. All libsquish calls go
// through SquishArchive, which serializes them on the handle.

using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Input;
using Microsoft.Win32;
using WinSquish.Interop;
using WinSquish.Models;
using WinSquish.Mvvm;
using WinSquish.Shell;
using WinSquish.Views;

namespace WinSquish.ViewModels;

public sealed class MainViewModel : ObservableObject
{
    private SquishArchive? _archive;
    private ArchiveNode? _root;

    public MainViewModel()
    {
        OpenCommand = new RelayCommand(async _ => await OpenAsync());
        NewCommand = new RelayCommand(async _ => await CreateAsync());
        CloseArchiveCommand = new RelayCommand(_ => CloseArchive(), _ => IsArchiveOpen);
        RefreshCommand = new RelayCommand(async _ => await ReopenAsync(), _ => IsArchiveOpen && !IsBusy);
        UpCommand = new RelayCommand(_ => NavigateUp(), _ => CurrentFolder?.Parent != null);
        NavigateCommand = new RelayCommand(o => { if (o is ArchiveNode n) NavigateTo(n); });
        OpenItemCommand = new RelayCommand(async _ => await OpenSelectedAsync(), _ => CanActOnSelection);
        ExtractSelectedCommand = new RelayCommand(async _ => await ExtractSelectedAsync(), _ => CanActOnSelection);
        DeleteSelectedCommand = new RelayCommand(async _ => await DeleteSelectedAsync(), _ => CanActOnSelection);
        ExtractAllCommand = new RelayCommand(async _ => await ExtractAllAsync(), _ => IsArchiveOpen && !IsBusy);
        SaveAsCommand = new RelayCommand(async _ => await SaveSelectedAsAsync(), _ => SelectedSingleFile != null);
        AboutCommand = new RelayCommand(_ => ShowAbout());
        AssociateCommand = new RelayCommand(_ => AssociateForCurrentUser());
        SelectAllCommand = new RelayCommand(_ => SelectAllRequested?.Invoke(), _ => Items.Count > 0);
    }

    /// <summary>Raised when the user asks to select every row (Ctrl+A); the view obliges.</summary>
    public event Action? SelectAllRequested;

    // --- bound collections & selection -----------------------------------

    /// <summary>Single-element collection wrapping the root, for the TreeView.</summary>
    public ObservableCollection<ArchiveNode> TreeRoots { get; } = new();

    /// <summary>Children of the current folder — the details list.</summary>
    public ObservableCollection<ArchiveNode> Items { get; } = new();

    /// <summary>Breadcrumb from root to the current folder.</summary>
    public ObservableCollection<ArchiveNode> Breadcrumb { get; } = new();

    private ArchiveNode? _currentFolder;
    public ArchiveNode? CurrentFolder
    {
        get => _currentFolder;
        private set
        {
            if (Set(ref _currentFolder, value))
            {
                RebuildItems();
                RebuildBreadcrumb();
                RaiseCommandStates();
            }
        }
    }

    /// <summary>Called by the view when the TreeView selection changes.</summary>
    public void OnTreeSelected(ArchiveNode? node)
    {
        if (node is { IsDirectory: true })
            CurrentFolder = node;
    }

    private IList<ArchiveNode> _selectedItems = Array.Empty<ArchiveNode>();
    /// <summary>Rows selected in the details list (wired up from the view).</summary>
    public void SetSelectedItems(IList<ArchiveNode> items)
    {
        _selectedItems = items;
        RaiseCommandStates();
        OnPropertyChanged(nameof(SelectionSummary));
    }

    private ArchiveNode? SelectedSingleFile =>
        _selectedItems.Count == 1 && !_selectedItems[0].IsDirectory ? _selectedItems[0] : null;

    private bool CanActOnSelection => IsArchiveOpen && !IsBusy && _selectedItems.Count > 0;

    // --- status / busy ---------------------------------------------------

    public bool IsArchiveOpen => _archive != null;

    private bool _isBusy;
    public bool IsBusy
    {
        get => _isBusy;
        private set { if (Set(ref _isBusy, value)) RaiseCommandStates(); }
    }

    private string _busyTitle = "";
    public string BusyTitle { get => _busyTitle; private set => Set(ref _busyTitle, value); }

    private double _progress;
    public double Progress { get => _progress; private set => Set(ref _progress, value); }

    private bool _progressIndeterminate;
    public bool ProgressIndeterminate
    {
        get => _progressIndeterminate;
        private set => Set(ref _progressIndeterminate, value);
    }

    private string _status = $"Ready — libsquish {SquishNativeVersion}";
    public string Status { get => _status; private set => Set(ref _status, value); }

    private string _windowTitle = "WinSquish";
    public string WindowTitle { get => _windowTitle; private set => Set(ref _windowTitle, value); }

    public string ArchiveSummary
    {
        get
        {
            if (_archive is null || _root is null) return "No archive open";
            var info = _archive.Info;
            string saved = info.TotalSize == 0
                ? ""
                : $" · {Format.Saved((double)ArchiveStoredSize / info.TotalSize)} smaller";
            return $"{_root.FileCount} files · {Format.Bytes(info.TotalSize)}{saved}";
        }
    }

    public string SelectionSummary
    {
        get
        {
            if (_selectedItems.Count == 0) return "";
            ulong size = 0;
            foreach (var n in _selectedItems) size += n.Size;
            return $"    ·    {_selectedItems.Count} selected · {Format.Bytes(size)}";
        }
    }

    private ulong ArchiveStoredSize
    {
        get { ulong s = 0; if (_root != null) foreach (var c in _root.Children) s += c.StoredSize; return s; }
    }

    private static string SquishNativeVersion
    {
        get { try { return SquishNative.Version(); } catch { return "unavailable"; } }
    }

    // --- commands --------------------------------------------------------

    public ICommand OpenCommand { get; }
    public ICommand NewCommand { get; }
    public ICommand CloseArchiveCommand { get; }
    public ICommand RefreshCommand { get; }
    public ICommand UpCommand { get; }
    public ICommand NavigateCommand { get; }
    public ICommand OpenItemCommand { get; }
    public ICommand ExtractSelectedCommand { get; }
    public ICommand DeleteSelectedCommand { get; }
    public ICommand ExtractAllCommand { get; }
    public ICommand SaveAsCommand { get; }
    public ICommand AboutCommand { get; }
    public ICommand AssociateCommand { get; }
    public ICommand SelectAllCommand { get; }

    /// <summary>True when .sqsh currently resolves to WinSquish (drives the welcome hint).</summary>
    public bool IsAssociated
    {
        get { try { return ShellIntegration.IsRegistered(); } catch { return false; } }
    }

    // --- open ------------------------------------------------------------

    public async Task OpenAsync()
    {
        var dlg = new OpenFileDialog
        {
            Title = "Open SQUISH archive",
            Filter = "SQUISH archives (*.sqsh;*.sq)|*.sqsh;*.sq|All files (*.*)|*.*",
            CheckFileExists = true,
        };
        if (dlg.ShowDialog() == true)
            await LoadArchiveAsync(dlg.FileName);
    }

    public async Task LoadArchiveAsync(string path)
    {
        if (IsBusy) return;
        try
        {
            BusyTitle = $"Opening {Path.GetFileName(path)}…";
            IsBusy = true;
            ProgressIndeterminate = true;

            var archive = await Task.Run(() => SquishArchive.Open(path));
            var root = ArchiveNode.BuildTree(archive, Path.GetFileName(path));

            _archive?.Dispose();
            _archive = archive;
            _root = root;

            TreeRoots.Clear();
            TreeRoots.Add(root);
            root.IsExpanded = true;
            root.IsSelected = true;
            CurrentFolder = root;

            WindowTitle = $"{Path.GetFileName(path)} — WinSquish";
            Status = $"Opened {path}";
            OnPropertyChanged(nameof(IsArchiveOpen));
            OnPropertyChanged(nameof(ArchiveSummary));
            RaiseCommandStates();
        }
        catch (Exception ex)
        {
            Fail("Could not open archive", ex);
        }
        finally
        {
            IsBusy = false;
            ProgressIndeterminate = false;
        }
    }

    private async Task ReopenAsync()
    {
        if (_archive != null) await LoadArchiveAsync(_archive.ArchivePath);
    }

    private void CloseArchive()
    {
        _archive?.Dispose();
        _archive = null;
        _root = null;
        TreeRoots.Clear();
        Items.Clear();
        Breadcrumb.Clear();
        _currentFolder = null;
        _selectedItems = Array.Empty<ArchiveNode>();
        WindowTitle = "WinSquish";
        Status = "Closed archive";
        OnPropertyChanged(nameof(IsArchiveOpen));
        OnPropertyChanged(nameof(ArchiveSummary));
        OnPropertyChanged(nameof(SelectionSummary));
        OnPropertyChanged(nameof(CurrentFolder));
        RaiseCommandStates();
    }

    // --- navigation ------------------------------------------------------

    public void NavigateTo(ArchiveNode folder)
    {
        if (!folder.IsDirectory) return;
        folder.ExpandAncestors();
        folder.IsExpanded = true;
        folder.IsSelected = true; // reflected in the tree; drives OnTreeSelected
        CurrentFolder = folder;
    }

    /// <summary>Open a row: descend into a folder, or preview a file.</summary>
    public async Task ActivateAsync(ArchiveNode node)
    {
        if (node.IsDirectory) NavigateTo(node);
        else await OpenFileAsync(node);
    }

    private void NavigateUp()
    {
        if (CurrentFolder?.Parent is { } p) NavigateTo(p);
    }

    private void RebuildItems()
    {
        Items.Clear();
        if (_currentFolder != null)
            foreach (var child in _currentFolder.Children)
                Items.Add(child);
        OnPropertyChanged(nameof(SelectionSummary));
    }

    private void RebuildBreadcrumb()
    {
        Breadcrumb.Clear();
        var chain = new List<ArchiveNode>();
        for (var n = _currentFolder; n != null; n = n.Parent) chain.Add(n);
        chain.Reverse();
        foreach (var n in chain) Breadcrumb.Add(n);
    }

    // --- extraction ------------------------------------------------------

    private async Task ExtractAllAsync()
    {
        if (_archive is null) return;
        var dst = PickFolder("Extract all files to…");
        if (dst is null) return;

        await RunBusyAsync($"Extracting all files to {Path.GetFileName(dst)}…", progress =>
            Task.Run(() => _archive.ExtractSubtree(null, dst, progress)));

        Status = $"Extracted archive to {dst}";
        OfferOpenFolder(dst);
    }

    private async Task ExtractSelectedAsync()
    {
        if (_archive is null || _selectedItems.Count == 0) return;
        var dst = PickFolder("Extract selected items to…");
        if (dst is null) return;

        var targets = _selectedItems.ToList();
        await RunBusyAsync($"Extracting {targets.Count} item(s)…", progress => Task.Run(() =>
        {
            for (int i = 0; i < targets.Count; i++)
            {
                int index = i;
                _archive.ExtractSubtree(targets[index].FullPath, dst,
                    (done, total) => progress(done, total == 0 ? 1 : total));
            }
        }));

        Status = $"Extracted {targets.Count} item(s) to {dst}";
        OfferOpenFolder(dst);
    }

    private async Task SaveSelectedAsAsync()
    {
        if (_archive is null || SelectedSingleFile is not { } file) return;
        var dlg = new SaveFileDialog { Title = "Save file as", FileName = file.Name };
        if (dlg.ShowDialog() != true) return;

        await RunBusyAsync($"Saving {file.Name}…", _ => Task.Run(() =>
            _archive.ExtractToFile(file.Entry!, dlg.FileName)));
        Status = $"Saved {dlg.FileName}";
    }

    // --- preview ---------------------------------------------------------

    private async Task OpenSelectedAsync()
    {
        if (SelectedSingleFile is { } f) await OpenFileAsync(f);
        else if (_selectedItems.Count == 1 && _selectedItems[0].IsDirectory)
            NavigateTo(_selectedItems[0]);
    }

    private async Task OpenFileAsync(ArchiveNode file)
    {
        if (_archive is null || file.Entry is null) return;
        try
        {
            string dir = Path.Combine(Path.GetTempPath(), "WinSquish", "preview");
            Directory.CreateDirectory(dir);
            string dst = Path.Combine(dir, file.Name);

            await RunBusyAsync($"Opening {file.Name}…", _ => Task.Run(() =>
                _archive.ExtractToFile(file.Entry, dst)));

            Process.Start(new ProcessStartInfo(dst) { UseShellExecute = true });
            Status = $"Opened {file.Name}";
        }
        catch (Exception ex)
        {
            Fail("Could not open file", ex);
        }
    }

    // --- create ----------------------------------------------------------

    private async Task CreateAsync()
    {
        var src = PickFolder("Choose a folder to compress into a new archive…");
        if (src is null) return;

        var dlg = new SaveFileDialog
        {
            Title = "Create SQUISH archive",
            Filter = "SQUISH archive (*.sqsh)|*.sqsh|All files (*.*)|*.*",
            FileName = new DirectoryInfo(src).Name + ".sqsh",
        };
        if (dlg.ShowDialog() != true) return;
        string dst = dlg.FileName;

        var opts = CompressOptionsDialog.Ask(
            Application.Current.MainWindow, Path.GetFileName(src), SafeThreads());
        if (opts is not { } o) return;

        try
        {
            await RunBusyAsync($"Compressing {Path.GetFileName(src)}…", progress => Task.Run(() =>
                SquishArchive.Create(src, dst, threads: o.Threads, chunkSize: o.ChunkSize,
                    (done, total) => progress(done, total == 0 ? 1 : total))));

            Status = $"Created {dst}";
            if (MessageBox.Show($"Archive created:\n{dst}\n\nOpen it now?", "WinSquish",
                    MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
                await LoadArchiveAsync(dst);
        }
        catch (Exception ex)
        {
            Fail("Could not create archive", ex);
        }
    }

    // --- add files (drag-and-drop into an open archive) ------------------

    /// <summary>
    /// Add dropped files/folders to the open archive at the current folder.
    /// Uses libsquish's in-place merge: existing members are copied verbatim and
    /// only the added files are compressed — no full re-pack.
    /// </summary>
    public async Task AddPathsAsync(IReadOnlyList<string> paths)
    {
        if (_archive is null || _root is null || IsBusy) return;

        var sources = paths.Where(p => File.Exists(p) || Directory.Exists(p)).ToList();
        if (sources.Count == 0) return;

        var targetFolder = CurrentFolder ?? _root;
        string prefix = targetFolder.FullPath;          // "" for the root
        string archivePath = _archive.ArchivePath;
        string archiveName = Path.GetFileName(archivePath);

        // Existing file paths, for collision detection (case-insensitive, '/'-separated).
        var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in _archive.Entries)
            if (!e.IsDir) existing.Add(e.Path.Trim('/'));

        // Resolve each dropped item to its destination path inside the archive,
        // and collect the files that would land on an existing entry.
        var items = new List<(string Src, string Dest)>();
        var conflicts = new List<string>();
        foreach (var src in sources)
        {
            string trimmed = src.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            string name = Path.GetFileName(trimmed);
            if (string.IsNullOrEmpty(name)) continue;   // e.g. a drive root
            string dest = string.IsNullOrEmpty(prefix) ? name : $"{prefix}/{name}";
            items.Add((src, dest));

            if (Directory.Exists(src))
            {
                foreach (var f in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
                {
                    string d = $"{dest}/{Path.GetRelativePath(src, f).Replace('\\', '/')}";
                    if (existing.Contains(d)) conflicts.Add(d);
                }
            }
            else if (existing.Contains(dest)) conflicts.Add(dest);
        }
        if (items.Count == 0) return;

        // On a name clash, ask (per this drop) whether to overwrite or keep existing.
        bool overwrite = true;
        if (conflicts.Count > 0)
        {
            var shown = string.Join("\n", conflicts.Take(12).Select(c => "   •  " + c));
            if (conflicts.Count > 12) shown += $"\n   …  (+{conflicts.Count - 12} more)";
            var choice = MessageBox.Show(
                $"{conflicts.Count} dropped file(s) already exist in this archive:\n\n{shown}\n\n" +
                "Overwrite them with the dropped versions?\n\n" +
                "Yes — overwrite      No — keep existing, add only new      Cancel — don't add anything",
                "Add files to archive", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (choice == MessageBoxResult.Cancel) return;
            overwrite = choice == MessageBoxResult.Yes;
        }
        bool keepExisting = !overwrite;

        try
        {
            // Release our read handle so libsquish can replace the file on disk.
            _archive.Dispose();
            _archive = null;

            await RunBusyAsync(
                $"Adding {items.Count} item(s) to {archiveName}…",
                progress => Task.Run(() =>
                {
                    foreach (var it in items)
                        SquishArchive.Add(archivePath, it.Src, it.Dest, keepExisting,
                            threads: 0, progress: (d, t) => progress(d, t == 0 ? 1 : t));
                }));

            await LoadArchiveAsync(archivePath);
            NavigateToPath(prefix);
            Status = $"Added {items.Count} item(s) to {archiveName}";
        }
        catch (Exception ex)
        {
            Fail("Could not add files", ex);
            if (_archive is null && File.Exists(archivePath))
                await LoadArchiveAsync(archivePath);
        }
    }

    /// <summary>
    /// Delete the selected files/folders from the archive. Uses libsquish's
    /// in-place remove: survivors are copied verbatim, nothing is recompressed.
    /// </summary>
    public async Task DeleteSelectedAsync()
    {
        if (_archive is null || _root is null || IsBusy) return;

        var targets = _selectedItems.Where(n => !string.IsNullOrEmpty(n.FullPath)).ToList();
        if (targets.Count == 0) return;

        string what = targets.Count == 1 ? $"“{targets[0].Name}”" : $"{targets.Count} items";
        if (MessageBox.Show(
                $"Delete {what} from the archive?\n\nThis can't be undone.",
                "Delete from archive", MessageBoxButton.YesNo, MessageBoxImage.Warning)
            != MessageBoxResult.Yes)
            return;

        var relPaths = targets.Select(n => n.FullPath).ToList();
        string reopenPrefix = (CurrentFolder ?? _root).FullPath;
        string archivePath = _archive.ArchivePath;
        string archiveName = Path.GetFileName(archivePath);
        int count = targets.Count;

        try
        {
            _archive.Dispose();
            _archive = null;

            await RunBusyAsync(
                $"Deleting {count} item(s) from {archiveName}…",
                progress => Task.Run(() =>
                    SquishArchive.Remove(archivePath, relPaths,
                        (d, t) => progress(d, t == 0 ? 1 : t))));

            await LoadArchiveAsync(archivePath);
            NavigateToPath(reopenPrefix);
            Status = $"Deleted {count} item(s) from {archiveName}";
        }
        catch (Exception ex)
        {
            Fail("Could not delete", ex);
            if (_archive is null && File.Exists(archivePath))
                await LoadArchiveAsync(archivePath);
        }
    }

    private void NavigateToPath(string fullPath)
    {
        if (string.IsNullOrEmpty(fullPath) || _root is null) return;
        if (FindFolder(_root, fullPath) is { } node) NavigateTo(node);
    }

    private static ArchiveNode? FindFolder(ArchiveNode from, string fullPath)
    {
        if (from.FullPath == fullPath) return from;
        foreach (var c in from.Children)
            if (c.IsDirectory &&
                (fullPath == c.FullPath || fullPath.StartsWith(c.FullPath + "/", StringComparison.Ordinal)))
                if (FindFolder(c, fullPath) is { } hit) return hit;
        return null;
    }

    // --- command-line startup actions ------------------------------------

    /// <summary>Run whatever the command line requested once the window is up.</summary>
    public async Task RunStartupAsync(StartupOptions s)
    {
        switch (s.Mode)
        {
            case StartupMode.Open when !string.IsNullOrEmpty(s.Path) && File.Exists(s.Path):
                await LoadArchiveAsync(s.Path);
                break;
            case StartupMode.ExtractHere:
                await OneShotExtractAsync(s.Path, toSubfolder: false);
                break;
            case StartupMode.ExtractTo:
                await OneShotExtractAsync(s.Path, toSubfolder: true);
                break;
            case StartupMode.Compress:
                await OneShotCompressAsync(s.Path);
                break;
        }
    }

    private async Task OneShotExtractAsync(string? archivePath, bool toSubfolder)
    {
        if (string.IsNullOrEmpty(archivePath) || !File.Exists(archivePath))
        {
            Fail("Extract", new FileNotFoundException("Archive not found.", archivePath ?? ""));
            return;
        }
        string full = Path.GetFullPath(archivePath);
        string parent = Path.GetDirectoryName(full) ?? ".";
        string dst = toSubfolder ? Path.Combine(parent, Path.GetFileNameWithoutExtension(full)) : parent;

        try
        {
            Directory.CreateDirectory(dst);
            using var a = SquishArchive.Open(full);
            await RunBusyAsync($"Extracting {Path.GetFileName(full)}…", progress =>
                Task.Run(() => a.ExtractSubtree(null, dst, (d, t) => progress(d, t == 0 ? 1 : t))));
            OpenInExplorer(dst);
            ShutdownOneShot();
        }
        catch (Exception ex)
        {
            Fail("Could not extract archive", ex);
        }
    }

    private async Task OneShotCompressAsync(string? path)
    {
        if (string.IsNullOrEmpty(path))
        {
            Fail("Compress", new ArgumentException("Nothing to compress."));
            return;
        }
        string full = Path.GetFullPath(path);
        bool isDir = Directory.Exists(full);
        if (!isDir && !File.Exists(full))
        {
            Fail("Compress", new FileNotFoundException("Path not found.", full));
            return;
        }
        string dst = full.TrimEnd('\\', '/') + ".sqsh";

        var opts = CompressOptionsDialog.Ask(
            Application.Current.MainWindow, Path.GetFileName(full), SafeThreads());
        if (opts is not { } o) { ShutdownOneShot(); return; }

        try
        {
            await RunBusyAsync($"Compressing {Path.GetFileName(full)}…", progress => Task.Run(() =>
            {
                if (isDir)
                    SquishArchive.Create(full, dst, threads: o.Threads, chunkSize: o.ChunkSize,
                        (d, t) => progress(d, t == 0 ? 1 : t));
                else
                    SquishArchive.CompressFile(full, dst, threads: o.Threads, chunkSize: o.ChunkSize,
                        (d, t) => progress(d, t == 0 ? 1 : t));
            }));
            SelectInExplorer(dst);
            ShutdownOneShot();
        }
        catch (Exception ex)
        {
            Fail("Could not compress", ex);
        }
    }

    private static void OpenInExplorer(string dir) =>
        Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true });

    private static void SelectInExplorer(string file) =>
        Process.Start("explorer.exe", $"/select,\"{file}\"");

    private static void ShutdownOneShot() =>
        Application.Current.Shutdown(0);

    // --- drag-out to Explorer --------------------------------------------

    /// <summary>
    /// Extract the given nodes to a fresh temp folder and return the top-level
    /// paths to hand Explorer as a file-drop. Files land flat by name; folders
    /// keep their subtree. Returns empty if busy or nothing usable.
    /// </summary>
    public async Task<List<string>> ExtractForDragAsync(IReadOnlyList<ArchiveNode> nodes)
    {
        var results = new List<string>();
        if (_archive is null || IsBusy || nodes.Count == 0) return results;

        string root = Path.Combine(
            Path.GetTempPath(), "WinSquish", "drag", Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(root);

        try
        {
            await RunBusyAsync("Preparing files…", progress => Task.Run(() =>
            {
                foreach (var n in nodes)
                {
                    if (n.IsDirectory)
                        _archive.ExtractSubtree(n.FullPath, root, (d, t) => progress(d, t == 0 ? 1 : t));
                    else if (n.Entry is not null)
                        _archive.ExtractToFile(n.Entry, Path.Combine(root, n.Name));
                }
            }));
        }
        catch (Exception ex)
        {
            Fail("Could not prepare files for drag", ex);
            return results;
        }

        foreach (var n in nodes)
        {
            string p = n.IsDirectory
                ? Path.Combine(root, n.FullPath.Replace('/', Path.DirectorySeparatorChar))
                : Path.Combine(root, n.Name);
            if ((File.Exists(p) || Directory.Exists(p)) && !results.Contains(p))
                results.Add(p);
        }
        return results;
    }

    // --- file association -------------------------------------------------

    private void AssociateForCurrentUser()
    {
        try
        {
            if (ShellIntegration.IsRegistered())
            {
                ShellIntegration.Unregister(false);
                Status = "Removed the WinSquish file association";
            }
            else
            {
                ShellIntegration.Register(false);
                Status = "WinSquish is now the default for .sqsh files";
                MessageBox.Show(
                    "WinSquish is now associated with .sqsh and .sq files, and a " +
                    "“Compress to SQUISH archive” entry was added to the folder " +
                    "right-click menu (current user).",
                    "WinSquish", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            OnPropertyChanged(nameof(IsAssociated));
        }
        catch (Exception ex)
        {
            Fail("Could not update file association", ex);
        }
    }

    // --- busy plumbing ---------------------------------------------------

    private async Task RunBusyAsync(string title, Func<Action<ulong, ulong>, Task> work)
    {
        if (IsBusy) return;
        var ui = Application.Current.Dispatcher;
        try
        {
            BusyTitle = title;
            Progress = 0;
            ProgressIndeterminate = false;
            IsBusy = true;

            void Report(ulong done, ulong total)
            {
                double frac = total == 0 ? 0 : Math.Clamp((double)done / total, 0, 1);
                ui.BeginInvoke(() =>
                {
                    Progress = frac;
                    BusyTitle = $"{title}  ({Format.Bytes(done)} / {Format.Bytes(total)})";
                });
            }

            await work(Report);
        }
        finally
        {
            IsBusy = false;
            ProgressIndeterminate = false;
        }
    }

    // --- small helpers ---------------------------------------------------

    private static string? PickFolder(string title)
    {
        var dlg = new OpenFolderDialog { Title = title, Multiselect = false };
        return dlg.ShowDialog() == true ? dlg.FolderName : null;
    }

    private static void OfferOpenFolder(string dir)
    {
        if (MessageBox.Show($"Done.\n\nOpen {dir} in Explorer?", "WinSquish",
                MessageBoxButton.YesNo, MessageBoxImage.Information) == MessageBoxResult.Yes)
            Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true });
    }

    private void ShowAbout()
    {
        MessageBox.Show(
            $"WinSquish {typeof(MainViewModel).Assembly.GetName().Version?.ToString(3)}\n" +
            $"A file-manager for SQUISH archives.\n\n" +
            $"libsquish {SquishNativeVersion}\n" +
            $"Cores available: {SafeThreads()}\n\n" +
            "SQUISH © 2026 Paige Julianne Sullivan — GPL-3.0-or-later.",
            "About WinSquish", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private static int SafeThreads() { try { return SquishNative.squish_threads(); } catch { return 1; } }

    private void Fail(string what, Exception ex)
    {
        Status = $"{what}: {ex.Message}";
        MessageBox.Show(ex.Message, what, MessageBoxButton.OK, MessageBoxImage.Error);
    }

    private void RaiseCommandStates()
    {
        foreach (var c in new[]
                 {
                     CloseArchiveCommand, RefreshCommand, UpCommand, OpenItemCommand,
                     ExtractSelectedCommand, DeleteSelectedCommand, ExtractAllCommand, SaveAsCommand,
                 })
            (c as RelayCommand)?.RaiseCanExecuteChanged();
        OnPropertyChanged(nameof(ArchiveSummary));
    }
}
