// Copyright (C) 2026  Paige Julianne Sullivan
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Code-behind: wires the WPF controls (tree selection, list selection/activation,
// keyboard, drag-drop, breadcrumb rendering) to the view model. All real logic
// lives in MainViewModel; this file only translates UI events into VM calls.

using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using WinSquish.Models;
using WinSquish.ViewModels;

namespace WinSquish.Views;

public partial class MainWindow : Window
{
    public MainViewModel ViewModel { get; } = new();

    /// <summary>Startup action parsed from the command line (set by App before Show).</summary>
    public StartupOptions? Startup { get; set; }

    public MainWindow()
    {
        InitializeComponent();
        DataContext = ViewModel;
        ViewModel.Breadcrumb.CollectionChanged += OnBreadcrumbChanged;

        // Select-all (Ctrl+A / context menu) is applied to the actual control.
        ViewModel.SelectAllRequested += () =>
        {
            DetailsView.SelectAll();
            DetailsView.Focus();
        };

        // Run whatever the command line asked for (open / extract / compress).
        Loaded += async (_, _) =>
        {
            if (Startup is { } s) await ViewModel.RunStartupAsync(s);
        };
    }

    // --- drag files out to Explorer --------------------------------------
    private Point _dragStart;
    private bool _dragging;

    private void OnDetailsPreviewLeftDown(object sender, MouseButtonEventArgs e)
        => _dragStart = e.GetPosition(null);

    private async void OnDetailsMouseMove(object sender, MouseEventArgs e)
    {
        if (_dragging || e.LeftButton != MouseButtonState.Pressed) return;

        var pos = e.GetPosition(null);
        if (Math.Abs(pos.X - _dragStart.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(pos.Y - _dragStart.Y) < SystemParameters.MinimumVerticalDragDistance)
            return;

        var nodes = DetailsView.SelectedItems.Cast<ArchiveNode>().ToList();
        if (nodes.Count == 0) return;

        _dragging = true;
        try
        {
            var files = await ViewModel.ExtractForDragAsync(nodes);
            if (files.Count > 0)
            {
                var data = new DataObject(DataFormats.FileDrop, files.ToArray());
                DragDrop.DoDragDrop(DetailsView, data, DragDropEffects.Copy);
            }
        }
        finally
        {
            _dragging = false;
        }
    }

    // --- folder tree ------------------------------------------------------
    private void OnTreeSelectionChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
        => ViewModel.OnTreeSelected(e.NewValue as ArchiveNode);

    // --- details list -----------------------------------------------------
    private void OnDetailsSelectionChanged(object sender, SelectionChangedEventArgs e)
        => ViewModel.SetSelectedItems(DetailsView.SelectedItems.Cast<ArchiveNode>().ToList());

    private async void OnDetailsDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (DetailsView.SelectedItem is ArchiveNode node)
            await ViewModel.ActivateAsync(node);
    }

    private async void OnDetailsKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && DetailsView.SelectedItem is ArchiveNode node)
        {
            e.Handled = true;
            await ViewModel.ActivateAsync(node);
        }
        else if (e.Key == Key.Delete)
        {
            e.Handled = true;
            if (ViewModel.DeleteSelectedCommand.CanExecute(null))
                ViewModel.DeleteSelectedCommand.Execute(null);
        }
    }

    // --- breadcrumb: render buttons + chevrons from the VM's chain ---------
    private void OnBreadcrumbChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        CrumbPanel.Children.Clear();
        var crumbs = ViewModel.Breadcrumb;
        for (int i = 0; i < crumbs.Count; i++)
        {
            var node = crumbs[i];
            bool isLast = i == crumbs.Count - 1;

            var btn = new Button
            {
                Style = (Style)FindResource("CrumbButton"),
                Content = node.Name,
                Command = ViewModel.NavigateCommand,
                CommandParameter = node,
            };
            if (isLast)
            {
                btn.Foreground = (System.Windows.Media.Brush)FindResource("TextPrimary");
                btn.FontWeight = FontWeights.SemiBold;
            }
            CrumbPanel.Children.Add(btn);

            if (!isLast)
            {
                CrumbPanel.Children.Add(new TextBlock
                {
                    Text = "", // chevron-right
                    FontFamily = (System.Windows.Media.FontFamily)FindResource("IconFont"),
                    FontSize = 11,
                    Margin = new Thickness(2, 0, 2, 0),
                    VerticalAlignment = VerticalAlignment.Center,
                    Foreground = (System.Windows.Media.Brush)FindResource("TextSecondary"),
                });
            }
        }
    }

    // --- drag & drop ------------------------------------------------------
    private void OnDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop)
            ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private async void OnDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } files) return;

        // With an archive open, a drop adds the files to it (at the current folder);
        // otherwise it opens the first dropped file as an archive.
        if (ViewModel.IsArchiveOpen)
            await ViewModel.AddPathsAsync(files);
        else
            await ViewModel.LoadArchiveAsync(files[0]);
    }
}
