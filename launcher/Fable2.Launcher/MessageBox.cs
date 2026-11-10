using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace Fable2Launcher;

public enum MessageBoxButton { OK }
public enum MessageBoxImage { Warning, Error }

/// <summary>
/// Minimal OK-only modal message box (WPF's MessageBox is not available in
/// Avalonia). Like the WPF original, Show blocks until the box is dismissed;
/// the launcher never needs the result, so only OK is provided.
/// </summary>
public static class MessageBox
{
    public static void Show(Window owner, string message, string title,
        MessageBoxButton button = MessageBoxButton.OK,
        MessageBoxImage image = MessageBoxImage.Warning)
    {
        var ok = new Button
        {
            Content = "OK",
            MinWidth = 96,
            HorizontalAlignment = HorizontalAlignment.Center,
            IsDefault = true,
        };
        var window = new Window
        {
            Title = title,
            SizeToContent = SizeToContent.WidthAndHeight,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            MaxWidth = 560,
            Content = new StackPanel
            {
                Margin = new Thickness(24),
                Spacing = 18,
                Children =
                {
                    new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
                    ok,
                },
            },
        };
        ok.Click += (_, _) => window.Close();

        // Modal and synchronous like WPF MessageBox.Show: run a nested
        // dispatcher frame until the dialog closes.
        var frame = new DispatcherFrame();
        window.Closed += (_, _) => frame.Continue = false;
        window.ShowDialog(owner);
        Dispatcher.UIThread.PushFrame(frame);
    }
}
