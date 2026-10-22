using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        Button _shareExportButton, _shareImportButton;
        void InitShareCode()
        {
            _shareExportButton = _window.FindName("ShareExportButton") as Button;
            _shareImportButton = _window.FindName("ShareImportButton") as Button;
            if (_shareExportButton != null) _shareExportButton.Click += delegate { ShowShareCode(false); };
            if (_shareImportButton != null) _shareImportButton.Click += delegate { ShowShareCode(true); };
        }
        void RefreshShareCode(bool enabled)
        {
            if (_shareExportButton != null) { _shareExportButton.Content = L("导出分享码", "Export share code"); _shareExportButton.IsEnabled = enabled; }
            if (_shareImportButton != null) { _shareImportButton.Content = L("导入分享码", "Import share code"); _shareImportButton.IsEnabled = enabled; }
        }
        string ShareError(Exception error)
        {
            var code = error as MochizukiShareCode.CodeException;
            if (code == null) return L("分享码操作失败，请检查文件权限、剪贴板与可用空间。", "Share-code operation failed. Check file permissions, clipboard and free space.");
            switch (code.Reason) {
                case "prefix": return L("分享码必须以 MZ1: 或 MZ2: 开头。", "Share code must start with MZ1: or MZ2:.");
                case "crc": return L("校验码不匹配，分享码可能已损坏。", "Checksum mismatch; the share code may be damaged.");
                case "index": return L("分享码包含未知参数编号，请更新启动器。", "Unknown parameter index. Update the launcher.");
                case "range": return L("参数超出允许范围。", "A parameter is outside its allowed range.");
                case "duplicate": return L("分享码包含重复参数编号。", "Duplicate parameter index in share code.");
                case "encoding": return L("INI 必须采用 UTF-8 或带 BOM 的 UTF-16 编码。", "INI must use UTF-8 or BOM-marked UTF-16 encoding.");
                case "value": return L("INI 中的风格参数值无效。", "Invalid visual-style value in the INI.");
                default: return L("分享码格式无效。", "Invalid share-code format.");
            }
        }
        void ShowShareCode(bool import)
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) return;
            string path = Path.Combine(game.Directory, "OptiScaler.ini");
            string executable = _selectedGame.Exe;
            if (!File.Exists(path)) { SetIniStatus("noini", ""); return; }
            try {
                var dialog = DialogTheme.Create(_window, import ? L("导入 Mochizuki 风格分享码", "Import Mochizuki visual style") : L("导出 Mochizuki 风格分享码", "Export Mochizuki visual style"), 620);
                var panel = new StackPanel { Margin = new Thickness(22) };
                panel.Children.Add(new TextBlock { Text = L("仅共享 Mochizuki 视觉风格，不更改安装、后端或其他 INI 设置。", "Shares only Mochizuki visual style; installation, backend and other INI settings are unchanged."), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 10) });
                var input = new TextBox { IsReadOnly = !import, AcceptsReturn = import, TextWrapping = TextWrapping.Wrap, Height = 130, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
                var includeGame = new CheckBox { Content = L("包含游戏标识（推荐）", "Include game tag (recommended)"), IsChecked = true, Margin = new Thickness(0, 10, 0, 0) };
                var summary = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 10, 0, 10) };
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
                var close = new Button { Content = L("关闭", "Close"), MinWidth = 75, Margin = new Thickness(6) };
                close.Click += delegate { dialog.Close(); };
                var action = new Button { Content = import ? L("预览", "Preview") : L("复制", "Copy"), MinWidth = 90, Margin = new Thickness(6) };
                var apply = new Button { Content = L("应用", "Apply"), MinWidth = 75, Margin = new Thickness(6), IsEnabled = false };
                string validated = null, validatedGame = "";
                input.TextChanged += delegate { validated = null; validatedGame = ""; apply.IsEnabled = false; summary.Text = ""; };
                if (!import) {
                    byte[] exportedIni = File.ReadAllBytes(path);
                    Action updateExport = delegate {
                        input.Text = MochizukiShareCode.Encode(exportedIni, includeGame.IsChecked == true ? executable : null);
                        summary.Text = MochizukiShareCode.Summary(MochizukiShareCode.Decode(input.Text), L("zh", "en") == "zh");
                        // An all-defaults export is a sparse code whose CRC is the empty-payload value;
                        // without this note it reads like a broken code full of placeholder Fs.
                        if (MochizukiShareCode.DifferenceCount(exportedIni, input.Text) == 0)
                            summary.Text += L("\n当前全部为默认值：分享码有效，导入方应用后将恢复同样的默认画面。",
                                               "\nEverything is at defaults: the code is valid; importing it restores the same defaults.");
                    };
                    includeGame.Checked += delegate { updateExport(); };
                    includeGame.Unchecked += delegate { updateExport(); };
                    updateExport();
                }
                action.Click += delegate {
                    try {
                        if (!import) { Clipboard.SetText(input.Text); return; }
                        if (!CanUseIniGame(game)) return;
                        var code = input.Text.Trim();
                        var values = MochizukiShareCode.Decode(code);
                        int difference = MochizukiShareCode.DifferenceCount(File.ReadAllBytes(path), code);
                        summary.Text = MochizukiShareCode.Summary(values, L("zh", "en") == "zh") + L("\n将更改 ", "\nWill change ") + difference + L(" 项设置（其余风格参数恢复默认）。", " settings (remaining visual parameters reset to defaults).");
                        validated = code; validatedGame = values.GameTag; apply.IsEnabled = true;
                    } catch (Exception ex) { validated = null; apply.IsEnabled = false; summary.Text = ShareError(ex); }
                };
                apply.Click += delegate {
                    if (validated == null || !CanUseIniGame(game)) return;
                    if (validatedGame.Length > 0 && !MochizukiShareCode.GameTagMatches(validatedGame, executable) &&
                        DialogTheme.Show(dialog,
                            L("该分享码来自「", "This share code was made for '") + validatedGame +
                            L("」，与当前游戏不符，仍要导入吗？", "', which does not match the current game. Import anyway?"),
                            L("游戏标识不匹配", "Game tag mismatch"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
                    if (ApplyShareCode(game, validated)) dialog.Close();
                };
                buttons.Children.Add(close); buttons.Children.Add(action); if (import) buttons.Children.Add(apply);
                panel.Children.Add(input); if (!import) panel.Children.Add(includeGame); panel.Children.Add(summary); panel.Children.Add(buttons);
                DialogTheme.Primary(dialog, action); DialogTheme.SetContent(dialog, panel);
                dialog.Loaded += delegate { input.Focus(); input.SelectAll(); };
                dialog.ShowDialog();
            } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); }
        }
        bool ApplyShareCode(IniGameIdentity game, string code)
        {
            if (!CanUseIniGame(game)) return false;
            bool locked = false, busy = false; string temporary = null;
            try {
                string path = Path.Combine(game.Directory, "OptiScaler.ini");
                if (!File.Exists(path)) { SetIniStatus("noini", ""); return false; }
                locked = TryLockTarget(game.Directory);
                if (!locked) { SetIniStatus("locked", ""); return false; }
                SetBusy(true, "share-code"); busy = true; RefreshProfileActions();
                byte[] bytes = MochizukiShareCode.Apply(File.ReadAllBytes(path), code);
                temporary = path + ".stage-" + Guid.NewGuid().ToString("N");
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { stream.Write(bytes, 0, bytes.Length); stream.Flush(true); }
                string backup = path + ".bak-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8);
                File.Replace(temporary, path, backup);
                SetIniStatus("share-imported", Path.GetFileName(backup)); return true;
            } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); return false; }
            finally {
                if (temporary != null && File.Exists(temporary)) { try { File.Delete(temporary); } catch { } }
                if (locked) ReleaseLock();
                if (busy) { SetBusy(false, ""); UpdatePrimaryButtonState(); }
                RefreshProfileActions();
            }
        }
    }
}
