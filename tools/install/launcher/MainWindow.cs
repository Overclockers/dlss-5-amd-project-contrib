using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    public sealed class GameItemViewModel
    {
        public GameInfo Info { get; set; }
        public string Name { get { return Info != null ? Info.Name : ""; } }
        public string Initial
        {
            get
            {
                if (string.IsNullOrEmpty(Name)) return "?";
                return Name.Substring(0, 1).ToUpperInvariant();
            }
        }
        public string SourceText { get { return Info != null ? Info.Source : ""; } }
        public string StatusText { get { return (Info != null && Info.Installed) ? "已安装" : "未安装"; } }
        public Brush StatusBrush
        {
            get
            {
                if (Info != null && Info.Installed)
                    return (Brush)new BrushConverter().ConvertFromString("#FF4ADE80");
                return (Brush)new BrushConverter().ConvertFromString("#FF9AA3B2");
            }
        }
    }

    public sealed class MainController
    {
        readonly Window _window;
        TextBox _searchBox, _logView, _packageRootBox;
        Button _libraryTab, _settingsTab, _addButton, _primaryButton, _changeExeButton, _uninstallButton, _openLogButton, _refreshButton, _packageBrowse;
        FrameworkElement _libraryPage, _settingsPage;
        ComboBox _filterCombo, _proxyCombo, _activeCombo, _langCombo;
        ListBox _gameList;
        TextBlock _heroTitle, _heroMeta, _heroPath, _injectText, _lmxxfReason, _danielReason, _mochiReason, _statusText, _countText;
        CheckBox _lmxxfCheck, _danielCheck, _mochiCheck, _replaceIni, _scanOnStartup;
        ProgressBar _progress;

        WorkerSession _session;
        Mutex _targetMutex;
        readonly string _settingsDir;
        readonly string _settingsPath;
        readonly string _libraryPath;
        string _language = "zh";
        bool _isScanOnStartup = true;
        string _packageRoot = "";
        string _pendingAction = "";
        string _currentLogPath = "";
        DateTime _shownUtc;

        readonly List<GameInfo> _allGames = new List<GameInfo>();
        GameInfo _selectedGame;
        LauncherResult _lastInspectResult;
        Decisions _decisions = new Decisions();

        static readonly Brush ReadyBrush = (Brush)new BrushConverter().ConvertFromString("#FF4ADE80");
        static readonly Brush MissingBrush = (Brush)new BrushConverter().ConvertFromString("#FF9AA3B2");
        static readonly Brush ActiveBrush = (Brush)new BrushConverter().ConvertFromString("#FFE8EEFF");

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
            _searchBox = (TextBox)window.FindName("SearchBox");
            _libraryTab = (Button)window.FindName("LibraryTab");
            _settingsTab = (Button)window.FindName("SettingsTab");
            _libraryPage = (FrameworkElement)window.FindName("LibraryPage");
            _settingsPage = (FrameworkElement)window.FindName("SettingsPage");
            _filterCombo = (ComboBox)window.FindName("FilterCombo");
            _addButton = (Button)window.FindName("AddButton");
            _gameList = (ListBox)window.FindName("GameList");
            _heroTitle = (TextBlock)window.FindName("HeroTitle");
            _heroMeta = (TextBlock)window.FindName("HeroMeta");
            _heroPath = (TextBlock)window.FindName("HeroPath");
            _primaryButton = (Button)window.FindName("PrimaryButton");
            _progress = (ProgressBar)window.FindName("Progress");
            _injectText = (TextBlock)window.FindName("InjectText");
            _proxyCombo = (ComboBox)window.FindName("ProxyCombo");
            _changeExeButton = (Button)window.FindName("ChangeExeButton");
            _lmxxfCheck = (CheckBox)window.FindName("LmxxfCheck");
            _lmxxfReason = (TextBlock)window.FindName("LmxxfReason");
            _danielCheck = (CheckBox)window.FindName("DanielCheck");
            _danielReason = (TextBlock)window.FindName("DanielReason");
            _mochiCheck = (CheckBox)window.FindName("MochiCheck");
            _mochiReason = (TextBlock)window.FindName("MochiReason");
            _activeCombo = (ComboBox)window.FindName("ActiveCombo");
            _replaceIni = (CheckBox)window.FindName("ReplaceIni");
            _uninstallButton = (Button)window.FindName("UninstallButton");
            _openLogButton = (Button)window.FindName("OpenLogButton");
            _logView = (TextBox)window.FindName("LogView");
            _statusText = (TextBlock)window.FindName("StatusText");
            _countText = (TextBlock)window.FindName("CountText");
            _langCombo = (ComboBox)window.FindName("LangCombo");
            _scanOnStartup = (CheckBox)window.FindName("ScanOnStartup");
            _refreshButton = (Button)window.FindName("RefreshButton");
            _packageRootBox = (TextBox)window.FindName("PackageRootBox");
            _packageBrowse = (Button)window.FindName("PackageBrowse");

            _settingsDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OptScalerNR", "Launcher");
            _settingsPath = Path.Combine(_settingsDir, "settings.json");
            _libraryPath = Path.Combine(_settingsDir, "library.json");

            _decisions.ExistingInstall = "overwrite";
            _decisions.ExistingIni = "overwrite";
            _decisions.MissingMochizukiModel = "skip";
            _decisions.ShortFileContinue = "abort";
            _decisions.ForeignProxy = new Dictionary<string, string>();

            if (_libraryTab != null) _libraryTab.Click += (s, e) => SwitchTab(true);
            if (_settingsTab != null) _settingsTab.Click += (s, e) => SwitchTab(false);

            if (_filterCombo != null)
            {
                _filterCombo.Items.Add("全部");
                _filterCombo.Items.Add("已安装");
                _filterCombo.Items.Add("未安装");
                _filterCombo.SelectedIndex = 0;
                _filterCombo.SelectionChanged += (s, e) => RefreshGameListView();
            }

            if (_searchBox != null)
                _searchBox.TextChanged += (s, e) => RefreshGameListView();

            if (_proxyCombo != null)
            {
                _proxyCombo.Items.Add("请选择");
                foreach (var name in new[] { "dxgi.dll", "winmm.dll", "d3d12.dll", "winhttp.dll", "wininet.dll", "dbghelp.dll" })
                    _proxyCombo.Items.Add(name);
                _proxyCombo.SelectedIndex = 0;
                _proxyCombo.SelectionChanged += (s, e) => UpdatePrimaryButtonState();
            }

            if (_langCombo != null)
            {
                _langCombo.Items.Add("zh");
                _langCombo.Items.Add("en");
                _langCombo.SelectedIndex = 0;
                _langCombo.SelectionChanged += (s, e) =>
                {
                    _language = Convert.ToString(_langCombo.SelectedItem) ?? "zh";
                    SaveSettings();
                    ApplyLang();
                };
            }

            if (_scanOnStartup != null)
            {
                _scanOnStartup.Checked += (s, e) => { _isScanOnStartup = true; SaveSettings(); };
                _scanOnStartup.Unchecked += (s, e) => { _isScanOnStartup = false; SaveSettings(); };
            }

            if (_packageBrowse != null) _packageBrowse.Click += (s, e) => BrowsePackageRoot();
            if (_packageRootBox != null)
            {
                _packageRootBox.LostKeyboardFocus += (s, e) =>
                {
                    _packageRoot = (_packageRootBox.Text ?? "").Trim();
                    SaveSettings();
                };
            }

            if (_addButton != null) _addButton.Click += (s, e) => AddGame();
            if (_gameList != null) _gameList.SelectionChanged += (s, e) => OnGameSelected();
            if (_changeExeButton != null) _changeExeButton.Click += (s, e) => ChangeExe();
            if (_primaryButton != null) _primaryButton.Click += (s, e) => OnPrimaryAction();
            if (_uninstallButton != null) _uninstallButton.Click += (s, e) => OnUninstall();
            if (_openLogButton != null) _openLogButton.Click += (s, e) => OpenLog();
            if (_refreshButton != null) _refreshButton.Click += (s, e) => RunScan();

            RoutedEventHandler backendChanged = (s, e) => { UpdateActiveCombo(); UpdatePrimaryButtonState(); };
            if (_lmxxfCheck != null) { _lmxxfCheck.Checked += backendChanged; _lmxxfCheck.Unchecked += backendChanged; }
            if (_danielCheck != null) { _danielCheck.Checked += backendChanged; _danielCheck.Unchecked += backendChanged; }
            if (_mochiCheck != null) { _mochiCheck.Checked += backendChanged; _mochiCheck.Unchecked += backendChanged; }

            _window.AllowDrop = true;
            _window.Drop += OnDrop;
            _window.DragOver += (s, e) => { e.Effects = DragDropEffects.Copy; e.Handled = true; };

            _window.Closing += OnClosing;
            _window.Loaded += (s, e) => OnFirstShown(args);

            LoadSettings();
            LoadLibraryCache();
            ApplyLang();
            ShowVersion();
            RefreshGameListView();
            UpdatePrimaryButtonState();
        }

        void SwitchTab(bool isLibrary)
        {
            if (_libraryPage != null) _libraryPage.Visibility = isLibrary ? Visibility.Visible : Visibility.Collapsed;
            if (_settingsPage != null) _settingsPage.Visibility = isLibrary ? Visibility.Collapsed : Visibility.Visible;
        }

        void OnFirstShown(string[] args)
        {
            _shownUtc = DateTime.UtcNow;
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
                    if (a != null && a.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && GamePaths.CanUse(a))
                    {
                        AddGameFromPath(a);
                    }
                }
            }

            if (_isScanOnStartup)
            {
                RunScan();
            }
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
                        return;
                    }
                }
            }
            catch { }
        }

        void ApplyLang()
        {
            var isZh = _language == "zh";
            if (_libraryTab != null) _libraryTab.Content = isZh ? "游戏库" : "Library";
            if (_settingsTab != null) _settingsTab.Content = isZh ? "设置" : "Settings";
            if (_addButton != null) _addButton.Content = isZh ? "+ 添加游戏" : "+ Add Game";
            if (_changeExeButton != null) _changeExeButton.Content = isZh ? "更换游戏 EXE" : "Change EXE";
            if (_uninstallButton != null) _uninstallButton.Content = isZh ? "卸载" : "Uninstall";
            if (_openLogButton != null) _openLogButton.Content = isZh ? "打开完整日志" : "Open Full Log";
            if (_refreshButton != null) _refreshButton.Content = isZh ? "刷新游戏库" : "Refresh Library";
            if (_packageBrowse != null) _packageBrowse.Content = isZh ? "浏览…" : "Browse…";

            if (_filterCombo != null && _filterCombo.Items.Count >= 3)
            {
                var idx = _filterCombo.SelectedIndex;
                _filterCombo.Items.Clear();
                _filterCombo.Items.Add(isZh ? "全部" : "All");
                _filterCombo.Items.Add(isZh ? "已安装" : "Installed");
                _filterCombo.Items.Add(isZh ? "未安装" : "Not Installed");
                _filterCombo.SelectedIndex = idx >= 0 ? idx : 0;
            }

            UpdatePrimaryButtonState();
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
                if (_langCombo != null && _langCombo.Items.Contains(_language)) _langCombo.SelectedItem = _language;
                _isScanOnStartup = rec.ScanOnStartup;
                if (_scanOnStartup != null) _scanOnStartup.IsChecked = _isScanOnStartup;
                if (!string.IsNullOrEmpty(rec.PackageRoot))
                {
                    _packageRoot = rec.PackageRoot;
                    if (_packageRootBox != null) _packageRootBox.Text = _packageRoot;
                }
            }
            catch { }
        }

        void SaveSettings()
        {
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var set = new GuiSettings
                {
                    Language = _language,
                    ScanOnStartup = _isScanOnStartup,
                    PackageRoot = _packageRoot,
                    RecentGameId = _selectedGame != null ? _selectedGame.Id : null
                };
                JsonUtil.WriteFile(_settingsPath, set);
            }
            catch { }
        }

        void LoadLibraryCache()
        {
            try
            {
                if (!File.Exists(_libraryPath)) return;
                var text = File.ReadAllText(_libraryPath);
                var rec = JsonUtil.Read<LibraryCache>(text);
                if (rec != null && rec.Games != null)
                {
                    _allGames.Clear();
                    foreach (var g in rec.Games)
                    {
                        if (g != null && !string.IsNullOrEmpty(g.Exe) && GamePaths.CanUse(g.Exe))
                            _allGames.Add(g);
                    }
                }
            }
            catch { }
        }

        void SaveLibraryCache()
        {
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var cache = new LibraryCache { Games = _allGames.ToArray() };
                var tmp = _libraryPath + ".tmp";
                JsonUtil.WriteFile(tmp, cache);
                File.Copy(tmp, _libraryPath, true);
                File.Delete(tmp);
            }
            catch { }
        }

        [System.Runtime.Serialization.DataContract]
        sealed class GuiSettings
        {
            [System.Runtime.Serialization.DataMember] public string Language;
            [System.Runtime.Serialization.DataMember] public bool ScanOnStartup = true;
            [System.Runtime.Serialization.DataMember] public string PackageRoot;
            [System.Runtime.Serialization.DataMember] public string RecentGameId;
        }

        [System.Runtime.Serialization.DataContract]
        sealed class LibraryCache
        {
            [System.Runtime.Serialization.DataMember] public GameInfo[] Games;
        }

        string WorkerScript
        {
            get
            {
                var baseDir = AppDomain.CurrentDomain.BaseDirectory;
                if (!string.IsNullOrEmpty(_packageRoot))
                {
                    var custom = Path.Combine(_packageRoot, "launcher-worker.ps1");
                    if (File.Exists(custom)) return custom;
                }
                var packaged = Path.Combine(baseDir, "launcher-worker.ps1");
                var fromTree = Path.GetFullPath(Path.Combine(baseDir, "..", "..", "tools", "install", "launcher-worker.ps1"));
                var mock = Path.Combine(baseDir, "mock-worker.ps1");
                if (File.Exists(packaged)) return packaged;
                if (File.Exists(fromTree)) return fromTree;
                if (File.Exists(mock)) return mock;
                throw new FileNotFoundException("Complete package required: launcher-worker.ps1 is missing.");
            }
        }

        void BrowsePackageRoot()
        {
            var dlg = new OpenFileDialog
            {
                Title = "Select complete package folder (pick Setup.ps1 or OptiScaler.dll)",
                Filter = "Package files (Setup.ps1;OptiScaler.dll)|Setup.ps1;OptiScaler.dll|All files (*.*)|*.*",
                CheckFileExists = true
            };
            if (dlg.ShowDialog(_window) == true)
            {
                _packageRoot = Path.GetDirectoryName(dlg.FileName);
                if (_packageRootBox != null) _packageRootBox.Text = _packageRoot;
                SaveSettings();
                if (_selectedGame != null) InspectGame(_selectedGame);
            }
        }

        void AddGame()
        {
            var picked = GamePaths.PickExe();
            if (string.IsNullOrEmpty(picked)) return;
            AddGameFromPath(picked);
        }

        void AddGameFromPath(string exePath)
        {
            var norm = GamePaths.Normalize(exePath);
            if (string.IsNullOrEmpty(norm) || !GamePaths.CanUse(norm)) return;

            GameInfo found = null;
            foreach (var g in _allGames)
            {
                if (string.Equals(g.Exe, norm, StringComparison.OrdinalIgnoreCase))
                {
                    found = g;
                    break;
                }
            }
            if (found == null)
            {
                var dir = Path.GetDirectoryName(norm);
                var folderName = Path.GetFileName(dir);
                var name = Path.GetFileNameWithoutExtension(norm);
                if (!string.IsNullOrEmpty(folderName) && !folderName.Equals("bin", StringComparison.OrdinalIgnoreCase)
                    && !folderName.Equals("win64", StringComparison.OrdinalIgnoreCase) && !folderName.Equals("wingdk", StringComparison.OrdinalIgnoreCase))
                {
                    name = folderName;
                }
                found = new GameInfo
                {
                    Id = "custom:" + norm.ToLowerInvariant(),
                    Name = name,
                    Exe = norm,
                    Source = "Added",
                    Installed = File.Exists(Path.Combine(dir, "amd-presr-install.txt"))
                };
                _allGames.Add(found);
                SaveLibraryCache();
            }

            RefreshGameListView();
            SelectGame(found);
        }

        void ChangeExe()
        {
            if (_selectedGame == null) return;
            var picked = GamePaths.PickExe();
            if (string.IsNullOrEmpty(picked)) return;
            var norm = GamePaths.Normalize(picked);
            if (string.IsNullOrEmpty(norm) || !GamePaths.CanUse(norm)) return;

            _selectedGame.Exe = norm;
            var dir = Path.GetDirectoryName(norm);
            _selectedGame.Installed = File.Exists(Path.Combine(dir, "amd-presr-install.txt"));
            SaveLibraryCache();

            RefreshGameListView();
            SelectGame(_selectedGame);
        }

        void OnDrop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files == null || files.Length == 0) return;
            var path = files[0];
            if (Directory.Exists(path))
            {
                var exes = Directory.GetFiles(path, "*.exe", SearchOption.TopDirectoryOnly);
                if (exes.Length > 0) path = exes[0];
            }
            if (path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                AddGameFromPath(path);
            }
        }

        void RefreshGameListView()
        {
            var query = (_searchBox != null ? _searchBox.Text : "").Trim().ToLowerInvariant();
            var filterIdx = _filterCombo != null ? _filterCombo.SelectedIndex : 0; // 0=all, 1=installed, 2=not installed

            var list = new List<GameItemViewModel>();
            foreach (var g in _allGames)
            {
                if (filterIdx == 1 && !g.Installed) continue;
                if (filterIdx == 2 && g.Installed) continue;
                if (!string.IsNullOrEmpty(query))
                {
                    var matchName = g.Name != null && g.Name.ToLowerInvariant().Contains(query);
                    var matchExe = g.Exe != null && g.Exe.ToLowerInvariant().Contains(query);
                    if (!matchName && !matchExe) continue;
                }
                list.Add(new GameItemViewModel { Info = g });
            }

            if (_gameList != null)
            {
                _gameList.ItemsSource = list;
                if (_selectedGame != null)
                {
                    for (int i = 0; i < list.Count; i++)
                    {
                        if (list[i].Info != null && string.Equals(list[i].Info.Id, _selectedGame.Id, StringComparison.OrdinalIgnoreCase))
                        {
                            _gameList.SelectedIndex = i;
                            break;
                        }
                    }
                }
            }

            if (_countText != null)
            {
                _countText.Text = _language == "zh"
                    ? string.Format("{0} 款游戏", list.Count)
                    : string.Format("{0} games", list.Count);
            }
        }

        void OnGameSelected()
        {
            if (_gameList == null) return;
            var item = _gameList.SelectedItem as GameItemViewModel;
            if (item != null && item.Info != null)
            {
                SelectGame(item.Info);
            }
        }

        void SelectGame(GameInfo game)
        {
            _selectedGame = game;
            if (_heroTitle != null) _heroTitle.Text = game.Name;
            if (_heroMeta != null) _heroMeta.Text = game.Source + " · PC";
            if (_heroPath != null) _heroPath.Text = game.Exe;

            UpdatePrimaryButtonState();
            InspectGame(game);
        }

        void UpdatePrimaryButtonState()
        {
            if (_primaryButton == null) return;
            var isZh = _language == "zh";
            if (_selectedGame == null)
            {
                _primaryButton.Content = isZh ? "选择一个游戏" : "Select a Game";
                _primaryButton.IsEnabled = false;
                if (_uninstallButton != null) _uninstallButton.IsEnabled = false;
                return;
            }

            var hasBackend = (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true)
                || (_danielCheck != null && _danielCheck.IsChecked == true)
                || (_mochiCheck != null && _mochiCheck.IsChecked == true);

            var proxyChosen = _proxyCombo != null && _proxyCombo.SelectedIndex > 0;
            var isBusy = _session != null && _session.IsRunning;

            _primaryButton.Content = _selectedGame.Installed ? (isZh ? "更新" : "Update") : (isZh ? "安装" : "Install");
            _primaryButton.IsEnabled = !isBusy && hasBackend && proxyChosen;
            if (_uninstallButton != null)
                _uninstallButton.IsEnabled = !isBusy && _selectedGame.Installed;
        }

        void UpdateActiveCombo()
        {
            if (_activeCombo == null) return;
            var checkedBackends = new List<string>();
            if (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true) checkedBackends.Add("lmxxf");
            if (_danielCheck != null && _danielCheck.IsChecked == true) checkedBackends.Add("daniel");
            if (_mochiCheck != null && _mochiCheck.IsChecked == true) checkedBackends.Add("mochizuki");

            var currentActive = Convert.ToString(_activeCombo.SelectedItem);
            _activeCombo.Items.Clear();
            foreach (var b in checkedBackends) _activeCombo.Items.Add(b);

            if (checkedBackends.Count >= 2)
            {
                _activeCombo.IsEnabled = true;
                if (checkedBackends.Contains(currentActive))
                    _activeCombo.SelectedItem = currentActive;
                else
                    _activeCombo.SelectedIndex = 0;
            }
            else if (checkedBackends.Count == 1)
            {
                _activeCombo.SelectedIndex = 0;
                _activeCombo.IsEnabled = false;
            }
            else
            {
                _activeCombo.IsEnabled = false;
            }
        }

        void RunScan()
        {
            if (_session != null && _session.IsRunning) return;
            SetBusy(true, "scan");
            if (_statusText != null) _statusText.Text = _language == "zh" ? "扫描游戏库中…" : "Scanning library…";
            if (_progress != null) _progress.IsIndeterminate = true;

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = "scan",
                PackageRoot = _packageRoot
            };

            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                _session = WorkerSession.Start(WorkerScript, req, work, canKill: true);
                _session.EventReceived += OnEvent;
                _session.Completed += OnScanCompleted;
            }
            catch (Exception ex)
            {
                SetBusy(false, "");
                AppendLog("Scan error: " + ex.Message);
            }
        }

        void OnScanCompleted(WorkerSession source, LauncherResult result, int code)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (!ReferenceEquals(_session, source))
                {
                    Detach(source);
                    return;
                }
                Detach(source);
                _session = null;
                SetBusy(false, "");

                if (result != null && result.Games != null)
                {
                    var map = new Dictionary<string, GameInfo>(StringComparer.OrdinalIgnoreCase);
                    foreach (var g in _allGames)
                    {
                        if (g != null && !string.IsNullOrEmpty(g.Exe))
                            map[g.Exe] = g;
                    }
                    foreach (var g in result.Games)
                    {
                        if (g == null || string.IsNullOrEmpty(g.Exe)) continue;
                        if (!map.ContainsKey(g.Exe))
                        {
                            map[g.Exe] = g;
                            _allGames.Add(g);
                        }
                        else
                        {
                            map[g.Exe].Installed = g.Installed;
                        }
                    }
                    SaveLibraryCache();
                    RefreshGameListView();
                }

                if (_statusText != null)
                    _statusText.Text = _language == "zh" ? "游戏库已更新" : "Library updated";
            }));
        }

        void InspectGame(GameInfo game)
        {
            if (game == null || string.IsNullOrEmpty(game.Exe)) return;
            if (_session != null && _session.IsRunning)
            {
                if (_session.CanKill)
                {
                    var old = _session;
                    old.EventReceived -= OnEvent;
                    old.Completed -= OnCompleted;
                    old.CancelInspect();
                    Detach(old);
                    _session = null;
                }
                else return;
            }

            SetBusy(true, "inspect");
            if (_injectText != null) _injectText.Text = _language == "zh" ? "正在检查导入与后端…" : "Checking imports and backends…";
            if (_progress != null) _progress.IsIndeterminate = true;

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = "inspect",
                GameExe = game.Exe,
                ProxyChoice = "auto",
                BackendChoice = "auto",
                PackageRoot = _packageRoot
            };

            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                _session = WorkerSession.Start(WorkerScript, req, work, canKill: true);
                _session.EventReceived += OnEvent;
                _session.Completed += OnCompleted;
            }
            catch (Exception ex)
            {
                SetBusy(false, "");
                AppendLog("Inspect failed: " + ex.Message);
            }
        }

        void OnPrimaryAction()
        {
            if (_selectedGame == null) return;
            var action = _selectedGame.Installed ? "update" : "install";
            ExecuteAction(action);
        }

        void OnUninstall()
        {
            if (_selectedGame == null) return;
            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            var isZh = _language == "zh";
            var msg = isZh
                ? ("确定要从以下目录卸载 OptScaler NR 吗？\n" + dir + "\n\n仅删除属于本项目的组件与代理文件。游戏原有文件和第三方插件会被完整保留。")
                : ("Uninstall OptScaler NR from:\n" + dir + "\n\nOwned files are removed. Unowned plugins and the original game files stay.");

            var confirm = MessageBox.Show(_window, msg, _window.Title, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (confirm != MessageBoxResult.OK) return;

            ExecuteAction("uninstall");
        }

        void ExecuteAction(string action)
        {
            if (_selectedGame == null || string.IsNullOrEmpty(_selectedGame.Exe)) return;
            if (_session != null && _session.IsRunning)
            {
                MessageBox.Show(_window, _language == "zh" ? "操作正在进行中…" : "An operation is already in progress.", _window.Title);
                return;
            }

            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            if (!TryLockTarget(dir))
            {
                if (_statusText != null)
                    _statusText.Text = _language == "zh" ? "目标文件夹已被另一个安装进程锁定。" : "Target folder is locked by another launcher.";
                return;
            }

            var proxy = Convert.ToString(_proxyCombo.SelectedItem);
            if (action != "uninstall" && (string.IsNullOrEmpty(proxy) || proxy == "请选择"))
            {
                MessageBox.Show(_window, _language == "zh" ? "请先选择要注入的代理 DLL。" : "Please select a proxy DLL to inject first.", _window.Title);
                ReleaseLock();
                return;
            }

            var checkedBackends = new List<string>();
            if (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true) checkedBackends.Add("lmxxf");
            if (_danielCheck != null && _danielCheck.IsChecked == true) checkedBackends.Add("daniel");
            if (_mochiCheck != null && _mochiCheck.IsChecked == true) checkedBackends.Add("mochizuki");

            if (action != "uninstall" && checkedBackends.Count == 0)
            {
                MessageBox.Show(_window, _language == "zh" ? "请至少勾选一个可安装的后端。" : "Please select at least one backend to install.", _window.Title);
                ReleaseLock();
                return;
            }

            SetBusy(true, action);
            if (_logView != null) _logView.Clear();
            if (_progress != null) _progress.IsIndeterminate = true;
            _pendingAction = action;

            _decisions.ExistingIni = (_replaceIni != null && _replaceIni.IsChecked == true) ? "overwrite" : "keep";

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = action,
                GameExe = _selectedGame.Exe,
                ProxyChoice = proxy,
                Backends = checkedBackends.ToArray(),
                ActiveBackend = Convert.ToString(_activeCombo.SelectedItem),
                PackageRoot = _packageRoot,
                ExplicitDecisions = _decisions,
                Fingerprint = (action != "inspect" && _lastInspectResult != null) ? _lastInspectResult.Fingerprint : null
            };

            var work = Path.Combine(Path.GetTempPath(), "OptScalerNR-launcher", req.RequestId);
            try
            {
                _session = WorkerSession.Start(WorkerScript, req, work, canKill: false);
                _session.EventReceived += OnEvent;
                _session.Completed += OnCompleted;
            }
            catch (Exception ex)
            {
                SetBusy(false, "");
                ReleaseLock();
                if (_statusText != null) _statusText.Text = ex.Message;
                AppendLog(ex.Message);
            }
        }

        void OnEvent(string stage, string message)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (_statusText != null) _statusText.Text = (stage ?? "") + ": " + (message ?? "");
                AppendLog((stage ?? "") + ": " + (message ?? ""));
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
                if (_progress != null) _progress.IsIndeterminate = false;
                SetBusy(false, "");

                if (result != null && !string.IsNullOrEmpty(result.LogPath))
                    _currentLogPath = result.LogPath;

                if (result == null)
                {
                    if (_statusText != null) _statusText.Text = "Worker failed (exit " + code + ").";
                }
                else if (string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase) && code != 0)
                {
                    if (_statusText != null) _statusText.Text = "Worker exit " + code + " does not match status " + result.Status;
                }
                else
                {
                    if (_statusText != null) _statusText.Text = result.Status + (string.IsNullOrEmpty(result.Error) ? "" : (": " + result.Error));
                }

                if (string.Equals(_pendingAction, "inspect", StringComparison.OrdinalIgnoreCase))
                {
                    ApplyInspect(result);
                }
                else if (result != null && string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase))
                {
                    if (_pendingAction == "install" || _pendingAction == "update")
                    {
                        if (_selectedGame != null) _selectedGame.Installed = true;
                    }
                    else if (_pendingAction == "uninstall")
                    {
                        if (_selectedGame != null) _selectedGame.Installed = false;
                    }
                    SaveLibraryCache();
                    RefreshGameListView();
                    if (_selectedGame != null) InspectGame(_selectedGame);
                }

                AppendLog(_statusText != null ? _statusText.Text : "");

                if (result != null && string.Equals(result.Status, "needsInput", StringComparison.OrdinalIgnoreCase))
                {
                    AskDecisions(result);
                }

                Detach(source);
                _session = null;
                ReleaseLock();
                UpdatePrimaryButtonState();
            }));
        }

        void ApplyInspect(LauncherResult result)
        {
            if (result == null) return;
            _lastInspectResult = result;

            if (_injectText != null)
            {
                if (!string.IsNullOrEmpty(result.Recommend))
                {
                    _injectText.Text = _language == "zh"
                        ? ("检测到程序导入 " + result.Recommend + "，建议尝试注入 " + result.Recommend + "。")
                        : ("Detected program import for " + result.Recommend + ", suggest injecting " + result.Recommend + ".");
                    if (_proxyCombo != null && _proxyCombo.Items.Contains(result.Recommend))
                        _proxyCombo.SelectedItem = result.Recommend;
                }
                else
                {
                    _injectText.Text = _language == "zh"
                        ? "无法判断，请选择 DLL 自行尝试。"
                        : "Cannot determine, please select a DLL manually.";
                    if (_proxyCombo != null) _proxyCombo.SelectedIndex = 0;
                }
            }

            if (result.Backends != null)
            {
                foreach (var b in result.Backends)
                {
                    if (b == null) continue;
                    if (b.Id == "lmxxf")
                    {
                        if (_lmxxfCheck != null)
                        {
                            _lmxxfCheck.IsEnabled = b.Enabled;
                            _lmxxfCheck.IsChecked = b.Enabled;
                        }
                        if (_lmxxfReason != null)
                        {
                            _lmxxfReason.Text = b.Reason;
                            _lmxxfReason.Foreground = b.Enabled ? ReadyBrush : MissingBrush;
                        }
                    }
                    else if (b.Id == "daniel")
                    {
                        if (_danielCheck != null)
                        {
                            _danielCheck.IsEnabled = b.Enabled;
                            _danielCheck.IsChecked = b.Enabled;
                        }
                        if (_danielReason != null)
                        {
                            _danielReason.Text = b.Reason;
                            _danielReason.Foreground = b.Enabled ? ReadyBrush : MissingBrush;
                        }
                    }
                    else if (b.Id == "mochizuki")
                    {
                        if (_mochiCheck != null)
                        {
                            _mochiCheck.IsEnabled = b.Enabled;
                            _mochiCheck.IsChecked = b.Enabled;
                        }
                        if (_mochiReason != null)
                        {
                            _mochiReason.Text = b.Reason;
                            _mochiReason.Foreground = b.Enabled ? ReadyBrush : MissingBrush;
                        }
                    }
                }
            }

            UpdateActiveCombo();
            UpdatePrimaryButtonState();

            if (result.Notes != null)
            {
                foreach (var n in result.Notes) AppendLog(n);
            }
        }

        bool AskDecisions(LauncherResult result)
        {
            if (result.RequiredDecisions == null) return false;
            foreach (var d in result.RequiredDecisions)
            {
                if (d.Id == "foreignProxy" && d.Options != null && d.Options.Length > 0)
                {
                    var prompt = d.Prompt + "\n\nYes = backup, No = keep, Cancel = abort.";
                    var pick = MessageBox.Show(_window, prompt, _window.Title, MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
                    if (_decisions.ForeignProxy == null) _decisions.ForeignProxy = new Dictionary<string, string>();
                    if (pick == MessageBoxResult.Yes) _decisions.ForeignProxy[d.File] = "backup";
                    else if (pick == MessageBoxResult.No) _decisions.ForeignProxy[d.File] = "keep";
                    else
                    {
                        if (_statusText != null) _statusText.Text = "Cancelled.";
                        return false;
                    }
                }
                else if (d.Id == "reinspect")
                {
                    if (_selectedGame != null) InspectGame(_selectedGame);
                    return true;
                }
            }
            ExecuteAction(string.IsNullOrEmpty(_pendingAction) ? "install" : _pendingAction);
            return true;
        }

        void OpenLog()
        {
            try
            {
                if (!string.IsNullOrEmpty(_currentLogPath) && File.Exists(_currentLogPath))
                {
                    Process.Start("notepad.exe", _currentLogPath);
                }
                else
                {
                    MessageBox.Show(_window, _language == "zh" ? "当前尚无可用日志。" : "No log file available yet.", _window.Title);
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(_window, ex.Message, _window.Title);
            }
        }

        bool TryLockTarget(string dir)
        {
            ReleaseLock();
            var hash = Convert.ToBase64String(SHA256.Create()
                .ComputeHash(Encoding.UTF8.GetBytes((dir ?? "").ToLowerInvariant())))
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

        void SetBusy(bool busy, string action)
        {
            if (_addButton != null) _addButton.IsEnabled = !busy;
            if (_changeExeButton != null) _changeExeButton.IsEnabled = !busy;
            if (_refreshButton != null) _refreshButton.IsEnabled = !busy;
            if (_primaryButton != null) _primaryButton.IsEnabled = !busy;
            if (_uninstallButton != null) _uninstallButton.IsEnabled = !busy && (_selectedGame != null && _selectedGame.Installed);
            if (_proxyCombo != null) _proxyCombo.IsEnabled = !busy;
            if (_lmxxfCheck != null) _lmxxfCheck.IsEnabled = !busy && (_lastInspectResult == null || IsBackendReady("lmxxf"));
            if (_danielCheck != null) _danielCheck.IsEnabled = !busy && (_lastInspectResult == null || IsBackendReady("daniel"));
            if (_mochiCheck != null) _mochiCheck.IsEnabled = !busy && (_lastInspectResult == null || IsBackendReady("mochizuki"));
        }

        bool IsBackendReady(string id)
        {
            if (_lastInspectResult == null || _lastInspectResult.Backends == null) return false;
            foreach (var b in _lastInspectResult.Backends)
            {
                if (b != null && b.Id == id) return b.Enabled;
            }
            return false;
        }

        void Detach(WorkerSession session)
        {
            if (session == null) return;
            session.EventReceived -= OnEvent;
            session.Completed -= OnCompleted;
            session.Completed -= OnScanCompleted;
            session.Dispose();
        }

        void AppendLog(string line)
        {
            if (_logView == null || string.IsNullOrEmpty(line)) return;
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
                    var msg = _language == "zh"
                        ? "写入操作正在进行中。强制关闭可能会中断文件更新。确定要关闭吗？"
                        : "A write is in progress. Closing now leaves the worker running until it finishes a safe step. Close anyway?";
                    var r = MessageBox.Show(_window, msg, _window.Title, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
                    if (r != MessageBoxResult.OK) e.Cancel = true;
                }
            }
            if (!e.Cancel) ReleaseLock();
        }
    }
}
