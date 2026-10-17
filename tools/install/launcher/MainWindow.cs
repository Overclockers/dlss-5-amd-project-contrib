using System;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed class MainController
    {
        readonly Window _window;
        TextBox _exePath, _logView;
        TextBlock _detect, _status;
        ComboBox _proxy, _backend;
        Button _browse, _install, _update, _uninstall;
        ProgressBar _progress;
        CheckBox _keepIni;
        WorkerSession _session;
        Mutex _targetMutex;
        string _lastInspected;
        LauncherResult _inspect;
        string _pendingAction;
        ComboBox _lang;
        TextBlock _title;
        readonly string _settingsDir;
        readonly string _settingsPath;
        string _language = "en";
        Decisions _decisions = new Decisions();
        DateTime _shownUtc;

        public static Window Create(string[] args)
        {
            Window window;
            using (var stream = Program.OpenXaml("MainWindow.xaml"))
                window = (Window)XamlReader.Load(stream);
            var controller = new MainController(window, args);
            window.Tag = controller;
            return window;
        }

        MainController(Window window, string[] args)
        {
            _window = window;
            _exePath = (TextBox)window.FindName("ExePath");
            _logView = (TextBox)window.FindName("LogView");
            _detect = (TextBlock)window.FindName("DetectSummary");
            _status = (TextBlock)window.FindName("StatusText");
            _proxy = (ComboBox)window.FindName("ProxyCombo");
            _backend = (ComboBox)window.FindName("BackendCombo");
            _browse = (Button)window.FindName("BrowseButton");
            _install = (Button)window.FindName("InstallButton");
            _update = (Button)window.FindName("UpdateButton");
            _uninstall = (Button)window.FindName("UninstallButton");
            _progress = (ProgressBar)window.FindName("Progress");
            _keepIni = (CheckBox)window.FindName("KeepIni");
            _lang = (ComboBox)window.FindName("LangCombo");
            _title = (TextBlock)window.FindName("TitleText");
            _settingsDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OptScalerNR", "Launcher");
            _settingsPath = Path.Combine(_settingsDir, "settings.json");
            _decisions.ExistingIni = "keep";
            _decisions.ExistingInstall = "overwrite";
            _decisions.MissingMochizukiModel = "skip";
            _decisions.ShortFileContinue = "abort";
            _decisions.ForeignProxy = new System.Collections.Generic.Dictionary<string, string>();

            foreach (var name in new[] { "auto", "dxgi.dll", "winmm.dll", "d3d12.dll", "winhttp.dll", "wininet.dll", "dbghelp.dll" })
                _proxy.Items.Add(name);
            _proxy.SelectedIndex = 0;
            foreach (var name in new[] { "auto", "lmxxf", "mochizuki", "daniel", "all" })
                _backend.Items.Add(name);
            _backend.SelectedIndex = 0;
            if (_lang != null)
            {
                _lang.Items.Add("en");
                _lang.Items.Add("zh");
                _lang.SelectedIndex = 0;
                _lang.SelectionChanged += (s, e) => { _language = Convert.ToString(_lang.SelectedItem) ?? "en"; SaveSettings(); ApplyLang(); };
            }

            _browse.Click += (s, e) => Browse();
            _install.Click += (s, e) => Run("install");
            _update.Click += (s, e) => Run("update");
            _uninstall.Click += (s, e) => Run("uninstall");
            _window.AllowDrop = true;
            _window.Drop += OnDrop;
            _window.DragOver += (s, e) => { e.Effects = DragDropEffects.Copy; e.Handled = true; };
            _exePath.LostKeyboardFocus += (s, e) => MaybeInspect();
            _exePath.KeyDown += (s, e) => { if (e.Key == Key.Enter) MaybeInspect(); };
            _window.Closing += OnClosing;
            _window.Loaded += (s, e) => OnFirstShown(args);
            LoadSettings();
            ApplyLang();
            ShowVersion();
            _install.IsEnabled = false;
            _update.IsEnabled = false;
            _uninstall.IsEnabled = false;
        }

        void OnFirstShown(string[] args)
        {
            _shownUtc = DateTime.UtcNow;
            var exeArg = FirstExistingExe(args);
            if (exeArg != null) _exePath.Text = exeArg;
            else if (!string.IsNullOrEmpty(_lastInspected) && GamePaths.CanUse(_lastInspected))
                _exePath.Text = _lastInspected;
            if (args != null)
            {
                foreach (var a in args)
                {
                    if (a == "--startup-measure")
                    {
                        WriteStartupMeasure();
                        _window.Close();
                        return;
                    }
                }
            }
            if (!string.IsNullOrEmpty(_exePath.Text)) MaybeInspect();
        }

        static string FirstExistingExe(string[] args)
        {
            if (args == null) return null;
            foreach (var a in args)
                if (a != null && a.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && GamePaths.CanUse(a))
                    return a;
            return null;
        }

        void ShowVersion()
        {
            try
            {
                foreach (var c in new[] {
                    Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "VERSION"),
                    Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "..", "VERSION") })
                {
                    if (File.Exists(c))
                    {
                        _window.Title = "OptScaler NR " + File.ReadAllText(c).Trim();
                        if (_title != null) _title.Text = _window.Title;
                        return;
                    }
                }
            }
            catch { }
        }

        void ApplyLang()
        {
            if (_browse != null) _browse.Content = _language == "zh" ? "浏览…" : "Browse…";
            if (_install != null) _install.Content = _language == "zh" ? "安装" : "Install";
            if (_update != null) _update.Content = _language == "zh" ? "更新" : "Update";
            if (_uninstall != null) _uninstall.Content = _language == "zh" ? "卸载" : "Uninstall";
        }

        void LoadSettings()
        {
            try
            {
                if (!File.Exists(_settingsPath)) return;
                var text = File.ReadAllText(_settingsPath);
                var rec = JsonUtil.Read<GuiSettings>(text);
                if (rec == null) return;
                if (!string.IsNullOrEmpty(rec.Language)) _language = rec.Language;
                if (_lang != null && _lang.Items.Contains(_language)) _lang.SelectedItem = _language;
                if (!string.IsNullOrEmpty(rec.RecentExe)) _lastInspected = rec.RecentExe;
            }
            catch { }
        }

        void SaveSettings()
        {
            try
            {
                Directory.CreateDirectory(_settingsDir);
                JsonUtil.WriteFile(_settingsPath, new GuiSettings { Language = _language, RecentExe = _exePath.Text });
            }
            catch { }
        }

        void WriteStartupMeasure()
        {
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var line = DateTime.UtcNow.ToString("o") + " interactiveMs=" +
                    (DateTime.UtcNow - Program.StartedUtc).TotalMilliseconds.ToString("0");
                File.AppendAllText(Path.Combine(_settingsDir, "startup.log"), line + Environment.NewLine);
                var outPath = Environment.GetEnvironmentVariable("OPTSCALER_STARTUP_LOG");
                if (!string.IsNullOrEmpty(outPath))
                    File.WriteAllText(outPath, line);
            }
            catch { }
        }

        [System.Runtime.Serialization.DataContract]
        sealed class GuiSettings
        {
            [System.Runtime.Serialization.DataMember] public string Language;
            [System.Runtime.Serialization.DataMember] public string RecentExe;
        }

        string PackageRoot
        {
            get { return Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "..")); }
        }

        string WorkerScript
        {
            get
            {
                var baseDir = AppDomain.CurrentDomain.BaseDirectory;
                var packaged = Path.Combine(baseDir, "launcher-worker.ps1");
                var fromTree = Path.GetFullPath(Path.Combine(baseDir, "..", "..", "tools", "install", "launcher-worker.ps1"));
                var mock = Path.Combine(baseDir, "mock-worker.ps1");
                if (File.Exists(packaged)) return packaged;
                if (File.Exists(fromTree)) return fromTree;
                if (File.Exists(mock)) return mock;
                throw new FileNotFoundException("Complete package required: launcher-worker.ps1 is missing.");
            }
        }

        void Browse()
        {
            var picked = GamePaths.PickExe();
            if (string.IsNullOrEmpty(picked)) return;
            _exePath.Text = picked;
            MaybeInspect();
        }

        void OnDrop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files == null || files.Length == 0) return;
            _exePath.Text = GamePaths.Normalize(files[0]);
            MaybeInspect();
        }

        void MaybeInspect()
        {
            var path = GamePaths.Normalize((_exePath.Text ?? "").Trim());
            if (path.Length == 0 || !GamePaths.CanUse(path)) return;
            _exePath.Text = path;
            if (string.Equals(path, _lastInspected, StringComparison.OrdinalIgnoreCase)
                && _inspect != null
                && string.Equals(_inspect.Status, "success", StringComparison.OrdinalIgnoreCase))
                return;
            if (_session != null && _session.IsRunning
                && string.Equals(path, _lastInspected, StringComparison.OrdinalIgnoreCase))
                return;
            Run("inspect");
        }

        void Detach(WorkerSession session)
        {
            if (session == null) return;
            session.EventReceived -= OnEvent;
            session.Completed -= OnCompleted;
            session.Dispose();
        }

        void Run(string action)
        {
            if (_session != null && _session.IsRunning)
            {
                if (action == "inspect" && _session.CanKill)
                {
                    var old = _session;
                    old.EventReceived -= OnEvent;
                    old.Completed -= OnCompleted;
                    old.CancelInspect();
                    Detach(old);
                    if (ReferenceEquals(_session, old)) _session = null;
                }
                else
                {
                    MessageBox.Show(_window, "An install or uninstall is already running.", _window.Title);
                    return;
                }
            }

            var exe = GamePaths.Normalize((_exePath.Text ?? "").Trim());
            if (exe.Length == 0 || !GamePaths.CanUse(exe))
            {
                _status.Text = "Select a game .exe first.";
                return;
            }
            if (action == "uninstall")
            {
                var confirm = MessageBox.Show(_window,
                    "Uninstall OptScaler NR from:\n" + Path.GetDirectoryName(exe) +
                    "\n\nOwned files are removed. Unowned plugins and the original game files stay.",
                    _window.Title, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
                if (confirm != MessageBoxResult.OK) return;
            }

            if (!TryLockTarget(Path.GetDirectoryName(exe)))
            {
                _status.Text = "Another launcher is already working on this folder.";
                return;
            }

            SetBusy(true, action != "inspect");
            _logView.Clear();
            _progress.IsIndeterminate = true;
            _pendingAction = action;
            _lastInspected = exe;
            _decisions.ExistingIni = _keepIni.IsChecked == true ? "keep" : "overwrite";
            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = action,
                GameExe = Path.GetFullPath(exe),
                ProxyChoice = Convert.ToString(_proxy.SelectedItem) ?? "auto",
                BackendChoice = Convert.ToString(_backend.SelectedItem) ?? "auto",
                ExplicitDecisions = _decisions,
                Fingerprint = action == "inspect" ? null : (_inspect != null ? _inspect.Fingerprint : null)
            };
            SaveSettings();
            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                var script = WorkerScript;
                _session = WorkerSession.Start(script, req, work, canKill: action == "inspect");
                _session.EventReceived += OnEvent;
                _session.Completed += OnCompleted;
            }
            catch (Exception ex)
            {
                SetBusy(false, false);
                _status.Text = ex.Message;
                AppendLog(ex.Message);
            }
        }

        bool TryLockTarget(string dir)
        {
            ReleaseLock();
            var hash = Convert.ToBase64String(System.Security.Cryptography.SHA256.Create()
                .ComputeHash(System.Text.Encoding.UTF8.GetBytes((dir ?? "").ToLowerInvariant())))
                .Replace('+', '-').Replace('/', '_').TrimEnd('=');
            var name = @"Global\OptScalerNR.Launcher." + hash;
            bool created;
            _targetMutex = new Mutex(true, name, out created);
            if (created) return true;
            _targetMutex.Dispose();
            _targetMutex = null;
            return false;
        }

        void ReleaseLock()
        {
            if (_targetMutex == null) return;
            try { _targetMutex.ReleaseMutex(); } catch { }
            _targetMutex.Dispose();
            _targetMutex = null;
        }

        void OnEvent(string stage, string message)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                _status.Text = (stage ?? "") + ": " + (message ?? "");
                AppendLog(_status.Text);
            }));
        }

        void OnCompleted(WorkerSession source, LauncherResult result, int code)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (!ReferenceEquals(_session, source))
                {
                    Detach(source);
                    return;
                }
                _progress.IsIndeterminate = false;
                SetBusy(false, false);
                if (result == null)
                    _status.Text = "Worker failed (exit " + code + ").";
                else if (string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase) && code != 0)
                    _status.Text = "Worker exit " + code + " does not match status " + result.Status;
                else
                    _status.Text = result.Status + (string.IsNullOrEmpty(result.Error) ? "" : (": " + result.Error));
                ApplyInspect(result);
                AppendLog(_status.Text);
                var retry = false;
                if (result != null && string.Equals(result.Status, "needsInput", StringComparison.OrdinalIgnoreCase))
                    retry = AskDecisions(result);
                var current = ReferenceEquals(_session, source);
                Detach(source);
                if (current) _session = null;
                if (_session == null) ReleaseLock();
            }));
        }

        void ApplyInspect(LauncherResult result)
        {
            if (result == null) return;
            var inspectOk = string.Equals(_pendingAction, "inspect", StringComparison.OrdinalIgnoreCase)
                && string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase);
            if (inspectOk || (result.Candidates != null && result.Candidates.Length > 0 && _inspect == null))
                _inspect = result;
            if (result.Candidates != null && result.Candidates.Length > 0)
            {
                var lines = new System.Text.StringBuilder();
                if (!string.IsNullOrEmpty(result.EffectiveGameDir))
                    lines.AppendLine("Folder: " + result.EffectiveGameDir);
                foreach (var c in result.Candidates)
                    lines.AppendLine(c.Name + " (" + c.Confidence + "): " + c.Reason);
                if (result.Conflicts != null)
                    foreach (var c in result.Conflicts)
                        lines.AppendLine("Conflict " + c.Name + ": " + c.Detail);
                _detect.Text = lines.ToString().Trim();
                if (!string.IsNullOrEmpty(result.ResolvedProxy) && _proxy.Items.Contains(result.ResolvedProxy))
                    _proxy.SelectedItem = result.ResolvedProxy;
            }
            if (result.Notes != null)
                foreach (var n in result.Notes) AppendLog(n);
            EnableActions(_inspect != null);
        }

        bool AskDecisions(LauncherResult result)
        {
            if (result.RequiredDecisions == null) return false;
            foreach (var d in result.RequiredDecisions)
            {
                if (d.Id == "foreignProxy" && d.Options != null && d.Options.Length > 0)
                {
                    var pick = MessageBox.Show(_window,
                        d.Prompt + "\n\nYes = backup, No = keep, Cancel = abort.",
                        _window.Title, MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
                    if (_decisions.ForeignProxy == null)
                        _decisions.ForeignProxy = new System.Collections.Generic.Dictionary<string, string>();
                    if (pick == MessageBoxResult.Yes) _decisions.ForeignProxy[d.File] = "backup";
                    else if (pick == MessageBoxResult.No) _decisions.ForeignProxy[d.File] = "keep";
                    else
                    {
                        _status.Text = "Cancelled.";
                        EnableActions(_inspect != null);
                        return false;
                    }
                }
                else if (d.Id == "reinspect")
                {
                    Run("inspect");
                    return true;
                }
                else
                {
                    _status.Text = "Cancelled.";
                    EnableActions(_inspect != null);
                    return false;
                }
            }
            Run(string.IsNullOrEmpty(_pendingAction) ? "install" : _pendingAction);
            return true;
        }

        void EnableActions(bool ready)
        {
            _install.IsEnabled = ready;
            _update.IsEnabled = ready;
            _uninstall.IsEnabled = ready;
        }

        void SetBusy(bool busy, bool writing)
        {
            _browse.IsEnabled = !busy;
            EnableActions(!busy && _inspect != null);
            _proxy.IsEnabled = !busy;
            _backend.IsEnabled = !busy;
        }

        void AppendLog(string line)
        {
            _logView.AppendText(line + Environment.NewLine);
            _logView.ScrollToEnd();
        }

        void OnClosing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (_session != null && _session.IsRunning)
            {
                if (_session.CanKill)
                {
                    _session.CancelInspect();
                    Detach(_session);
                    _session = null;
                }
                else
                {
                    var r = MessageBox.Show(_window,
                        "A write is in progress. Closing now leaves the worker running until it finishes a safe step. Close the window anyway?",
                        _window.Title, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
                    if (r != MessageBoxResult.OK) e.Cancel = true;
                }
            }
            if (!e.Cancel) ReleaseLock();
        }
    }
}
