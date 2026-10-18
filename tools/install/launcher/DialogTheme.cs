using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Shell;

namespace OptScalerNR.Launcher
{
    public static class DialogTheme
    {
        public static ResourceDictionary Resources()
        {
            using (var stream = Program.OpenXaml("DialogTheme.xaml")) return (ResourceDictionary)XamlReader.Load(stream);
        }
        public static Window Create(Window owner, string title, double width)
        {
            var window = new Window { Title = title, Width = width, SizeToContent = SizeToContent.Height };
            Apply(window, owner);
            return window;
        }
        public static void Apply(Window window, Window owner)
        {
            window.Owner = owner != null && owner.IsVisible ? owner : null;
            window.WindowStartupLocation = window.Owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen;
            window.ShowInTaskbar = false;
            window.ResizeMode = ResizeMode.NoResize;
            window.WindowStyle = WindowStyle.None;
            WindowChrome.SetWindowChrome(window, new WindowChrome { CaptionHeight = 0, GlassFrameThickness = new Thickness(0), ResizeBorderThickness = new Thickness(0) });
            window.Background = new SolidColorBrush(Color.FromRgb(22,24,32));
            window.Foreground = Brushes.WhiteSmoke;
            window.BorderBrush = new SolidColorBrush(Color.FromRgb(52,61,80));
            window.BorderThickness = new Thickness(1);
            window.FontFamily = owner != null ? owner.FontFamily : new FontFamily("Segoe UI, Microsoft YaHei UI");
            window.FontSize = 13;
            window.Language = owner != null ? owner.Language : XmlLanguage.GetLanguage("en-US");
            window.MaxHeight = Math.Max(300, Math.Min(800, SystemParameters.WorkArea.Height - 50));
            window.MaxWidth = Math.Max(300, SystemParameters.WorkArea.Width - 40);
            window.Resources.MergedDictionaries.Add(Resources());
            window.PreviewKeyDown += delegate(object sender, KeyEventArgs e) { if(e.Key == Key.Escape) { window.Close(); e.Handled = true; } };
        }
        public static void Primary(Window window, Button button)
        {
            button.Style = window.FindResource("DialogPrimary") as Style;
            button.IsDefault = true;
        }
        public static void SetContent(Window window, UIElement content)
        {
            var root = new DockPanel { Background = window.Background };
            var header = new Border { Padding = new Thickness(20,12,12,12), BorderThickness = new Thickness(0,0,0,1), BorderBrush = window.BorderBrush };
            var row = new DockPanel();
            var close = new Button { Content = "✕", Padding = new Thickness(10,4,10,4), MinHeight = 30, ToolTip = window.Language.IetfLanguageTag.StartsWith("zh") ? "关闭" : "Close" };
            close.Click += delegate { window.Close(); };
            DockPanel.SetDock(close, Dock.Right); row.Children.Add(close);
            row.Children.Add(new TextBlock { Text = window.Title, FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            header.Child = row;
            header.MouseLeftButtonDown += delegate(object sender, MouseButtonEventArgs e) { if (e.ButtonState == MouseButtonState.Pressed) { try { window.DragMove(); } catch { } } };
            DockPanel.SetDock(header, Dock.Top); root.Children.Add(header);
            var viewer = content as ScrollViewer;
            if (viewer == null) viewer = new ScrollViewer { Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
            viewer.MaxHeight = window.MaxHeight - 64;
            root.Children.Add(viewer);
            window.Content = root;
        }
        public static MessageBoxResult Show(Window owner, string text, string caption)
        { return Show(owner, text, caption, MessageBoxButton.OK, MessageBoxImage.Information); }
        public static MessageBoxResult Show(Window owner, string text, string caption, MessageBoxButton buttons)
        { return Show(owner, text, caption, buttons, MessageBoxImage.Information); }
        public static MessageBoxResult Show(Window owner, string text, string caption, MessageBoxButton buttons, MessageBoxImage image)
        {
            bool zh = owner != null && owner.Language.IetfLanguageTag.StartsWith("zh");
            var window = Create(owner, caption, 600);
            var panel = new StackPanel { Margin = new Thickness(22) };
            panel.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = image == MessageBoxImage.Error ? Brushes.LightSalmon : Brushes.WhiteSmoke, Margin = new Thickness(0,0,0,20) });
            var row = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right };
            var result = MessageBoxResult.Cancel;
            Action<string,MessageBoxResult,bool> add = (label, value, primary) => {
                var b = new Button { Content = label, MinWidth = 90, Margin = new Thickness(4) };
                if (primary) Primary(window, b);
                b.Click += delegate { result = value; window.DialogResult = true; };
                row.Children.Add(b);
            };
            if (buttons == MessageBoxButton.YesNo || buttons == MessageBoxButton.YesNoCancel) {
                if (buttons == MessageBoxButton.YesNoCancel) add(zh ? "取消" : "Cancel", MessageBoxResult.Cancel, false);
                add(zh ? "否" : "No", MessageBoxResult.No, false);
                add(zh ? "是" : "Yes", MessageBoxResult.Yes, true);
            } else {
                if (buttons == MessageBoxButton.OKCancel) add(zh ? "取消" : "Cancel", MessageBoxResult.Cancel, false);
                add(zh ? "确定" : "OK", MessageBoxResult.OK, true);
            }
            panel.Children.Add(row); SetContent(window, panel);
            window.ShowDialog();
            return result;
        }
    }
}
