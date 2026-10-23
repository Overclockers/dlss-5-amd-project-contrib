using System;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        Button _styleOutButton;
        Button _styleInButton;
        DispatcherTimer _styleOutResetTimer;

        void InitShareCode()
        {
            _styleOutButton = _window.FindName("StyleOutButton") as Button;
            if (_styleOutButton != null) _styleOutButton.Click += delegate { ShareMyLook(); };
            _styleInButton = _window.FindName("StyleInButton") as Button;
            if (_styleInButton != null) _styleInButton.Click += delegate { ShowImportLook(); };
        }

        void RefreshShareCode(bool enabled)
        {
            var title = _window.FindName("StyleShareTitle") as TextBlock;
            var sub = _window.FindName("StyleShareSub") as TextBlock;
            if (title != null) title.Text = L("同款神仙画质，一键抄作业", "Share & try custom looks");
            if (sub != null)
                sub.Text = L("调出了满意画面？发到群里晒一晒，或者直接换上大佬的同款调色",
                             "Show off your look in chat, or try a look from the community in one click");
            if (_styleOutButton != null)
            {
                _styleOutButton.Content = L("晒出我的画面", "Share My Look");
                _styleOutButton.ToolTip = L("生成一段可以发到群里、评论区的画面分享文本", "Copies a share text for chat and comments");
                _styleOutButton.IsEnabled = enabled;
            }
            if (_styleInButton != null)
            {
                _styleInButton.Content = L("换上同款画面", "Use a Shared Look");
                _styleInButton.ToolTip = L("粘贴别人分享的画面文本，一键换上同款", "Paste someone's shared look and wear it");
                _styleInButton.IsEnabled = enabled;
            }
        }

        // 晒出我的画面: zero-dialog export. The clipboard gets a self-explaining share text
        // (pitch + code) so the code carries its own instructions wherever it is forwarded.
        void ShareMyLook()
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) return;
            string path = Path.Combine(game.Directory, "OptiScaler.ini");
            if (!File.Exists(path)) { SetIniStatus("noini", ""); return; }
            try {
                string code = MochizukiShareCode.Encode(File.ReadAllBytes(path), _selectedGame.Exe);
                string title = MochizukiShareCode.NormalizeGameTag(_selectedGame.Exe);
                string pitch = title.Length > 0
                    ? L("【" + title + "】我用 Mochizuki 调的画质，启动器点「换上同款画面」粘贴就能用：\n",
                       "[" + title + "] My Mochizuki look — in the launcher hit \"Use a Shared Look\" and paste this:\n")
                    : L("我用 Mochizuki 调的画质，启动器点「换上同款画面」粘贴就能用：\n",
                       "My Mochizuki look — in the launcher hit \"Use a Shared Look\" and paste this:\n");
                Clipboard.SetText(pitch + code);
                if (_styleOutButton != null)
                {
                    _styleOutButton.Content = L("已复制！快去群里粘贴吧 ✓", "Copied! Go paste it ✓");
                    if (_styleOutResetTimer == null)
                    {
                        _styleOutResetTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
                        _styleOutResetTimer.Tick += delegate
                        {
                            _styleOutResetTimer.Stop();
                            if (_styleOutButton != null) _styleOutButton.Content = L("晒出我的画面", "Share My Look");
                        };
                    }
                    _styleOutResetTimer.Stop();
                    _styleOutResetTimer.Start();
                }
                SetIniStatus("share-copied", "");
            } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); }
        }

        // MZ code inside arbitrary pasted text (the share text we generate, chat quotes, anything)
        static readonly Regex CodeInText = new Regex(@"MZ[12]:[^;\r\n]*;[0-9A-Fa-f]{4}", RegexOptions.Compiled);
        static string ExtractCode(string text)
        {
            if (text == null) return null;
            var match = CodeInText.Match(text);
            return match.Success ? match.Value : null;
        }

        void ShowImportLook()
        {
            var game = CaptureIniGame();
            if (!CanUseIniGame(game)) return;
            string path = Path.Combine(game.Directory, "OptiScaler.ini");
            if (!File.Exists(path)) { SetIniStatus("noini", ""); return; }
            try {
                var dialog = DialogTheme.Create(_window, L("换上同款画面", "Use a Shared Look"), 620);
                var panel = new StackPanel { Margin = new Thickness(22) };
                var hint = new TextBlock
                {
                    Text = L("把群里或评论区复制来的分享内容粘贴在下面：", "Paste a shared look from chat or comments below:"),
                    TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8)
                };
                panel.Children.Add(hint);
                var input = new TextBox { TextWrapping = TextWrapping.Wrap, Height = 130, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
                panel.Children.Add(input);
                var summary = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 10, 0, 10) };
                panel.Children.Add(summary);
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
                var close = new Button { Content = L("关闭", "Close"), MinWidth = 75, Margin = new Thickness(6) };
                close.Click += delegate { dialog.Close(); };
                var apply = new Button { Content = L("立即换上这套画面", "Apply This Look"), MinWidth = 110, Margin = new Thickness(6), IsEnabled = false };
                string validated = null, validatedGame = "";

                Action validate = delegate {
                    validated = null; validatedGame = ""; apply.IsEnabled = false; summary.Text = "";
                    var code = ExtractCode(input.Text);
                    if (code == null)
                    {
                        if (input.Text.Trim().Length > 0)
                            summary.Text = L("没认出画质方案哦。确认复制了别人分享的完整内容（里面会有一段 MZ 开头的字符）。",
                                             "No shared look found here. Make sure you copied the full share text (including the MZ... part).");
                        return;
                    }
                    try {
                        var values = MochizukiShareCode.Decode(code);
                        string tag = values.GameTag.Length > 0 ? "【" + values.GameTag + "】" : "";
                        summary.Text = L("✨ 已识别" + tag + "同款画质（色调、质感等专属调校）",
                                         "✨ Ready: custom look" + (values.GameTag.Length > 0 ? " for [" + values.GameTag + "]" : "") + " (color and texture tuning)");
                        validated = code; validatedGame = values.GameTag; apply.IsEnabled = true;
                    } catch (Exception ex) { summary.Text = ShareError(ex); }
                };
                input.TextChanged += delegate { validate(); };
                apply.Click += delegate {
                    if (validated == null || !CanUseIniGame(game)) return;
                    if (validatedGame.Length > 0 && !MochizukiShareCode.GameTagMatches(validatedGame, _selectedGame.Exe) &&
                        DialogTheme.Show(dialog,
                            L("这段画质来自「" + validatedGame + "」，和当前游戏不是同一个，还要换上吗？",
                              "This look was made for '" + validatedGame + "', not this game. Apply anyway?"),
                            L("游戏不一样", "Different game"), MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
                    if (ApplyShareCode(game, validated)) dialog.Close();
                };
                buttons.Children.Add(close); buttons.Children.Add(apply);
                panel.Children.Add(buttons);
                DialogTheme.Primary(dialog, apply); DialogTheme.SetContent(dialog, panel);

                // Prefill only when the clipboard already holds a recognizable code — never dump
                // unrelated clipboard text into the box.
                try {
                    if (Clipboard.ContainsText()) {
                        var code = ExtractCode(Clipboard.GetText());
                        if (code != null) input.Text = code;
                    }
                } catch { }

                dialog.Loaded += delegate { if (input.Text.Length == 0) input.Focus(); };
                dialog.ShowDialog();
            } catch (Exception ex) { SetIniStatus("share-error", ShareError(ex)); }
        }

        string ShareError(Exception error)
        {
            var code = error as MochizukiShareCode.CodeException;
            if (code == null) return L("操作没成功，再试一次或检查文件权限。", "The operation failed. Try again or check file permissions.");
            switch (code.Reason) {
                case "prefix": return L("没认出画质方案哦。确认复制了别人分享的完整内容（里面会有一段 MZ 开头的字符）。",
                                        "No shared look found here. Make sure you copied the full share text (including the MZ... part).");
                case "crc": return L("内容好像没复制全，少了一截。把整段话重新复制一遍试试？",
                                     "Looks like part of the text was cut off. Try copying the whole thing again.");
                case "index": return L("这段画质来自更新的版本，更新启动器后再试。", "This look needs a newer launcher. Update first.");
                case "range": return L("这段画质的内容有问题，可能没复制全。", "The shared look looks incomplete.");
                case "duplicate": return L("这段画质的内容有问题，可能没复制全。", "The shared look looks incomplete.");
                case "encoding": return L("游戏设置文件不是常见的编码，无法读取。", "The game INI uses an unsupported encoding.");
                case "value": return L("这段画质的内容有问题，可能没复制全。", "The shared look looks incomplete.");
                default: return L("没认出画质方案哦。确认复制了别人分享的完整内容。", "No shared look recognized. Copy the full share text.");
            }
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
