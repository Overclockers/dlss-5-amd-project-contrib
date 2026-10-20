using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using Microsoft.Win32;
using Path = System.IO.Path;

namespace OptScalerNR.Launcher
{
    public sealed class GameItemViewModel
    {
        public GameInfo Info { get; set; }
        public bool IsZh { get; set; }
        public ImageSource IconSource { get; set; }

        public string Name { get { return Info != null ? Info.DisplayName : ""; } }
        public string Initial
        {
            get
            {
                if (string.IsNullOrEmpty(Name)) return "?";
                return Name.Substring(0, 1).ToUpperInvariant();
            }
        }
        public string SourceText { get { return Info != null ? Info.Source : ""; } }
        public string StatusText
        {
            get
            {
                if (Info != null && !Info.HasExecutable) return IsZh ? "◇ 待选择 EXE" : "◇ Select EXE";
                if (Info != null && Info.Installed)
                    return IsZh ? "● 已安装" : "● Installed";
                return IsZh ? "○ 未安装" : "○ Not Installed";
            }
        }
        public Brush StatusBrush
        {
            get
            {
                if (Info != null && Info.Installed)
                    return (Brush)new BrushConverter().ConvertFromString("#FF4ADE80");
                return (Brush)new BrushConverter().ConvertFromString("#FF8E95A5");
            }
        }
        public string Cover { get { return Info != null ? Info.DisplayCover : null; } }
        public bool HasCover
        {
            get
            {
                try { return !string.IsNullOrEmpty(Cover) && File.Exists(Cover); }
                catch { return false; }
            }
        }

        public ImageSource DisplayImage
        {
            get
            {
                if (HasCover)
                {
                    try
                    {
                        var bi = new BitmapImage();
                        bi.BeginInit();
                        bi.CacheOption = BitmapCacheOption.OnLoad;
                        bi.DecodePixelWidth = 84;
                        bi.UriSource = new Uri(Cover, UriKind.Absolute);
                        bi.EndInit();
                        return bi;
                    }
                    catch { }
                }
                return IconSource;
            }
        }

        public Visibility CoverVisibility { get { return (HasCover || IconSource != null) ? Visibility.Visible : Visibility.Collapsed; } }
        public Visibility InitialVisibility { get { return (HasCover || IconSource != null) ? Visibility.Collapsed : Visibility.Visible; } }
        public string Hero { get { return Info != null ? Info.DisplayHero : null; } }
    }

    public sealed partial class MainController
    {
        readonly Window _window;
        TextBox _searchBox, _logView, _packageRootBox;
        Button _minBtn, _maxBtn, _closeBtn;
        Button _libraryTab, _settingsTab, _addButton, _primaryButton, _openFolderButton, _uninstallButton, _openLogButton, _refreshButton, _langToggle, _packageBrowse;
        FrameworkElement _libraryPage, _settingsPage;
        ComboBox _filterCombo, _proxyCombo, _activeCombo, _langCombo;
        ListBox _gameList;
        TextBlock _pageTitle, _countBadge, _heroKicker, _heroTitle, _heroMeta, _heroPath, _heroPosterInitial, _injectText, _injectIcon, _injectRecommendation, _refundWarning;
        TextBlock _lmxxfReason, _danielReason, _mochiReason, _statusText, _countText, _versionText;
        Border _lmxxfBadge, _danielBadge, _mochiBadge;
        Button _lmxxfActiveBtn, _danielActiveBtn, _mochiActiveBtn;
        Button _compatButton, _restoreCompatButton, _compatHelpButton;
        MenuItem _removeGameMenuItem;
        TextBlock _compatSummary;
        TextBlock _lmxxfActiveLabel, _danielActiveLabel, _mochiActiveLabel;
        CheckBox _lmxxfCheck, _danielCheck, _mochiCheck, _replaceIni, _scanOnStartup;
        ProgressBar _progress;
        Image _heroPosterImage, _heroBgImage;
        Ellipse _statusLed;

        RadioButton _pillDxgi, _pillD3d12, _pillWinmm, _pillWinhttp, _pillWininet, _pillDbghelp;
        Dictionary<string, RadioButton> _proxyPills;

        WorkerSession _session;
        Mutex _targetMutex;
        readonly string _settingsDir;
        readonly string _settingsPath;
        readonly string _libraryPath;
        string _language = "en";
        bool _loadingSettings = true;
        bool _applyingLanguage;
        bool _saveInitialLanguage;
        string _productVersion = "";
        string _recentGameId;
        string _eventStage;
        bool _isScanOnStartup = true;
        string _packageRoot = "";
        string _pendingAction = "";
        bool _writeBusy;
        bool _updatingBackends;
        string _currentLogPath = "";
        string _activeBackend = "mochizuki";
        DateTime _shownUtc;

        readonly List<GameInfo> _allGames = new List<GameInfo>();
        readonly Dictionary<string, BackendChoice> _backendChoices = new Dictionary<string, BackendChoice>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _hiddenGames = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string _resumeInstallAction = "";
        GameInfo _selectedGame;
        LauncherResult _lastInspectResult;
        Decisions _decisions = new Decisions();

        static readonly Brush ReadyBrush = (Brush)new BrushConverter().ConvertFromString("#FF4ADE80");
        static readonly Brush ReadyBorder = (Brush)new BrushConverter().ConvertFromString("#FF22C55E");
        static readonly Brush ReadyBg = (Brush)new BrushConverter().ConvertFromString("#FF17281F");
        static readonly Brush MissingBrush = (Brush)new BrushConverter().ConvertFromString("#FF8E95A5");
        static readonly Brush MissingBorder = (Brush)new BrushConverter().ConvertFromString("#FF282D3A");
        static readonly Brush MissingBg = (Brush)new BrushConverter().ConvertFromString("#FF1E222E");
        static readonly Brush WarningBrush = (Brush)new BrushConverter().ConvertFromString("#FFFACC15");

        public static Window Create(string[] args)
        {
            return Create(args, null);
        }

        internal static Window Create(string[] args, string settingsDirectory)
        {
            Window window;
            using (var stream = Program.OpenXaml("MainWindow.xaml"))
                window = (Window)XamlReader.Load(stream);
            window.Resources.MergedDictionaries.Add(DialogTheme.Resources());
            var controller = new MainController(window, args, settingsDirectory);
            window.Tag = controller;
            return window;
        }

        MainController(Window window, string[] args, string settingsDirectory)
        {
            _window = window;

            _minBtn = (Button)window.FindName("MinBtn");
            _maxBtn = (Button)window.FindName("MaxBtn");
            _closeBtn = (Button)window.FindName("CloseBtn");

            _searchBox = (TextBox)window.FindName("SearchBox");
            _pageTitle = (TextBlock)window.FindName("PageTitle");
            _countBadge = (TextBlock)window.FindName("CountBadge");
            _refundWarning = (TextBlock)window.FindName("RefundWarning");
            _langToggle = (Button)window.FindName("LangToggle");
            _libraryTab = (Button)window.FindName("LibraryTab");
            _settingsTab = (Button)window.FindName("SettingsTab");
            _libraryPage = (FrameworkElement)window.FindName("LibraryPage");
            _settingsPage = (FrameworkElement)window.FindName("SettingsPage");
            _filterCombo = (ComboBox)window.FindName("FilterCombo");
            _addButton = (Button)window.FindName("AddButton");
            _gameList = (ListBox)window.FindName("GameList");

            _heroKicker = (TextBlock)window.FindName("HeroKicker");
            _heroTitle = (TextBlock)window.FindName("HeroTitle");
            _heroMeta = (TextBlock)window.FindName("HeroMeta");
            _heroPath = (TextBlock)window.FindName("HeroPath");
            _heroPosterInitial = (TextBlock)window.FindName("HeroPosterInitial");
            _heroPosterImage = (Image)window.FindName("HeroPosterImage");
            _heroBgImage = (Image)window.FindName("HeroBgImage");

            _primaryButton = (Button)window.FindName("PrimaryButton");
            _openFolderButton = (Button)window.FindName("OpenFolderButton");
            _uninstallButton = (Button)window.FindName("UninstallButton");
            _compatButton = (Button)window.FindName("CompatButton");
            _restoreCompatButton = (Button)window.FindName("RestoreCompatButton");
            _compatHelpButton = (Button)window.FindName("CompatHelpButton");
            _compatSummary = (TextBlock)window.FindName("CompatSummary");
            _progress = (ProgressBar)window.FindName("Progress");

            _injectText = (TextBlock)window.FindName("InjectText");
            _injectIcon = (TextBlock)window.FindName("InjectIcon");
            _injectRecommendation = (TextBlock)window.FindName("InjectRecommendation");
            _proxyCombo = (ComboBox)window.FindName("ProxyCombo");

            _pillDxgi = (RadioButton)window.FindName("PillDxgi");
            _pillD3d12 = (RadioButton)window.FindName("PillD3d12");
            _pillWinmm = (RadioButton)window.FindName("PillWinmm");
            _pillWinhttp = (RadioButton)window.FindName("PillWinhttp");
            _pillWininet = (RadioButton)window.FindName("PillWininet");
            _pillDbghelp = (RadioButton)window.FindName("PillDbghelp");

            _lmxxfCheck = (CheckBox)window.FindName("LmxxfCheck");
            _lmxxfBadge = (Border)window.FindName("LmxxfBadge");
            _lmxxfReason = (TextBlock)window.FindName("LmxxfReason");
            _lmxxfActiveBtn = (Button)window.FindName("LmxxfActiveBtn");
            _lmxxfActiveLabel = (TextBlock)window.FindName("LmxxfActiveLabel");

            _danielCheck = (CheckBox)window.FindName("DanielCheck");
            _danielBadge = (Border)window.FindName("DanielBadge");
            _danielReason = (TextBlock)window.FindName("DanielReason");
            _danielActiveBtn = (Button)window.FindName("DanielActiveBtn");
            _danielActiveLabel = (TextBlock)window.FindName("DanielActiveLabel");

            _mochiCheck = (CheckBox)window.FindName("MochiCheck");
            _mochiBadge = (Border)window.FindName("MochiBadge");
            _mochiReason = (TextBlock)window.FindName("MochiReason");
            _mochiActiveBtn = (Button)window.FindName("MochiActiveBtn");
            _mochiActiveLabel = (TextBlock)window.FindName("MochiActiveLabel");

            _activeCombo = (ComboBox)window.FindName("ActiveCombo");
            _replaceIni = (CheckBox)window.FindName("ReplaceIni");
            _openLogButton = (Button)window.FindName("OpenLogButton");
            _logView = (TextBox)window.FindName("LogView");
            _statusText = (TextBlock)window.FindName("StatusText");
            _statusLed = (Ellipse)window.FindName("StatusLed");
            _countText = (TextBlock)window.FindName("CountText");
            _versionText = (TextBlock)window.FindName("VersionText");

            _langCombo = (ComboBox)window.FindName("LangCombo");
            _scanOnStartup = (CheckBox)window.FindName("ScanOnStartup");
            _refreshButton = (Button)window.FindName("RefreshButton");
            _packageRootBox = (TextBox)window.FindName("PackageRootBox");
            _packageBrowse = (Button)window.FindName("PackageBrowse");
            var githubBtn = window.FindName("GithubButton") as Button;
            if (githubBtn != null) githubBtn.Click += (s, e) => OpenUrl("https://github.com/TheAutomatic/dlss-5-amd-project");
            var bilibiliBtn = window.FindName("BilibiliButton") as Button;
            if (bilibiliBtn != null) bilibiliBtn.Click += (s, e) => OpenUrl("https://space.bilibili.com/291088");

            _settingsDir = settingsDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OptScalerNR", "Launcher");
            _settingsPath = Path.Combine(_settingsDir, "settings.json");
            _libraryPath = Path.Combine(_settingsDir, "library.json");
            InitLibraryActions();
            InitRecommendations();

            _decisions.ExistingInstall = "overwrite";
            _decisions.ExistingIni = "overwrite";
            _decisions.MissingMochizukiModel = "skip";
            _decisions.ShortFileContinue = "abort";
            _decisions.ForeignProxy = new Dictionary<string, string>();

            if (_minBtn != null) _minBtn.Click += (s, e) => _window.WindowState = WindowState.Minimized;
            if (_maxBtn != null) _maxBtn.Click += (s, e) =>
            {
                _window.WindowState = (_window.WindowState == WindowState.Maximized) ? WindowState.Normal : WindowState.Maximized;
            };
            if (_closeBtn != null) _closeBtn.Click += (s, e) => _window.Close();

            _window.StateChanged += (s, e) =>
            {
                if (_window.WindowState == WindowState.Maximized)
                {
                    _window.BorderThickness = new Thickness(7);
                    if (_maxBtn != null) _maxBtn.Content = "\x25A3";
                }
                else
                {
                    _window.BorderThickness = new Thickness(1);
                    if (_maxBtn != null) _maxBtn.Content = "\x25A2";
                }
            };

            if (_libraryTab != null) _libraryTab.Click += (s, e) => SwitchTab(false);
            if (_settingsTab != null) _settingsTab.Click += (s, e) => SwitchTab(_settingsPage == null || _settingsPage.Visibility != Visibility.Visible);
            if (_langToggle != null) _langToggle.Click += (s, e) => ToggleLanguage();

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

            InitProxyPills();

            if (_proxyCombo != null)
            {
                _proxyCombo.Items.Add("请选择");
                foreach (var name in ProxyRecommendations.ProxyNames)
                    _proxyCombo.Items.Add(name);
                _proxyCombo.SelectedIndex = 0;
                _proxyCombo.SelectionChanged += (s, e) =>
                {
                    var selected = Convert.ToString(_proxyCombo.SelectedItem);
                    SelectProxyPillOnly(selected);
                    RememberProxyChoice();
                    UpdatePrimaryButtonState();
                };
            }

            if (_langCombo != null)
            {
                _langCombo.Items.Add(new ComboBoxItem { Tag = "zh", Content = "Chinese" });
                _langCombo.Items.Add(new ComboBoxItem { Tag = "en", Content = "English" });
                _langCombo.SelectionChanged += (s, e) =>
                {
                    if (_loadingSettings || _applyingLanguage) return;
                    var item = _langCombo.SelectedItem as ComboBoxItem;
                    if (item != null) SetLanguage(Convert.ToString(item.Tag));
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
            if (_openFolderButton != null) _openFolderButton.Click += (s, e) => OpenSelectedFolder();
            if (_primaryButton != null) _primaryButton.Click += (s, e) => OnPrimaryAction();
            if (_uninstallButton != null) _uninstallButton.Click += (s, e) => OnUninstall();
            InitGameContextMenu();
            if (_compatButton != null) _compatButton.Click += (s, e) => OnCompatibility(false);
            if (_restoreCompatButton != null) _restoreCompatButton.Click += (s, e) => OnCompatibility(true);
            if (_compatHelpButton != null) _compatHelpButton.Click += (s, e) => ShowCompatibilityHelp();
            if (_openLogButton != null) _openLogButton.Click += (s, e) => OpenLog();
            if (_refreshButton != null) _refreshButton.Click += (s, e) => RunScan();

            RoutedEventHandler backendChanged = (s, e) => {
                if (_updatingBackends) return;
                UpdateActiveCombo(); SaveBackendChoice(); UpdatePrimaryButtonState();
            };
            if (_lmxxfCheck != null) { _lmxxfCheck.Checked += backendChanged; _lmxxfCheck.Unchecked += backendChanged; }
            if (_danielCheck != null) { _danielCheck.Checked += backendChanged; _danielCheck.Unchecked += backendChanged; }
            if (_mochiCheck != null) { _mochiCheck.Checked += backendChanged; _mochiCheck.Unchecked += backendChanged; }

            if (_lmxxfActiveBtn != null) _lmxxfActiveBtn.Click += (s, e) => SetActiveBackend("lmxxf");
            if (_danielActiveBtn != null) _danielActiveBtn.Click += (s, e) => SetActiveBackend("daniel");
            if (_mochiActiveBtn != null) _mochiActiveBtn.Click += (s, e) => SetActiveBackend("mochizuki");

            _window.AllowDrop = true;
            _window.Drop += OnDrop;
            _window.DragOver += (s, e) => { e.Effects = DragDropEffects.Copy; e.Handled = true; };

            _window.Closing += OnClosing;
            _window.Loaded += (s, e) => OnFirstShown(args);

            LoadSettings();
            _loadingSettings = false;
            LoadLibraryCache();
            ShowVersion();
            ApplyLang();
            if (_saveInitialLanguage) SaveSettings();
            RefreshGameListView();
            UpdateActiveCombo();
            UpdatePrimaryButtonState();
        }

        void InitProxyPills()
        {
            _proxyPills = new Dictionary<string, RadioButton>(StringComparer.OrdinalIgnoreCase);
            if (_pillDxgi != null) _proxyPills["dxgi.dll"] = _pillDxgi;
            if (_pillD3d12 != null) _proxyPills["d3d12.dll"] = _pillD3d12;
            if (_pillWinmm != null) _proxyPills["winmm.dll"] = _pillWinmm;
            if (_pillWinhttp != null) _proxyPills["winhttp.dll"] = _pillWinhttp;
            if (_pillWininet != null) _proxyPills["wininet.dll"] = _pillWininet;
            if (_pillDbghelp != null) _proxyPills["dbghelp.dll"] = _pillDbghelp;
            var versionPill = (RadioButton)_window.FindName("PillVersion");
            if (versionPill != null) _proxyPills["version.dll"] = versionPill;

            foreach (var kvp in _proxyPills)
            {
                var dllName = kvp.Key;
                var pill = kvp.Value;
                pill.Checked += (s, e) =>
                {
                    if (_proxyCombo != null)
                    {
                        for (int i = 0; i < _proxyCombo.Items.Count; i++)
                        {
                            if (string.Equals(Convert.ToString(_proxyCombo.Items[i]), dllName, StringComparison.OrdinalIgnoreCase))
                            {
                                _proxyCombo.SelectedIndex = i;
                                break;
                            }
                        }
                    }
                    UpdatePrimaryButtonState();
                    RememberProxyChoice();
                };
            }
        }

        void SelectProxyPillOnly(string dllName)
        {
            if (_proxyPills == null) return;
            foreach (var kvp in _proxyPills)
            {
                if (string.Equals(kvp.Key, dllName, StringComparison.OrdinalIgnoreCase))
                {
                    if (kvp.Value.IsChecked != true) kvp.Value.IsChecked = true;
                }
            }
        }

        void SelectProxy(string dllName)
        {
            var previousSelectionMode = _selectingProxy;
            _selectingProxy = true;
            try {
            if (string.IsNullOrEmpty(dllName))
            {
                if (_proxyPills != null)
                {
                    foreach (var p in _proxyPills.Values) p.IsChecked = false;
                }
                if (_proxyCombo != null) _proxyCombo.SelectedIndex = 0;
                return;
            }

            if (_proxyPills != null && _proxyPills.ContainsKey(dllName))
            {
                _proxyPills[dllName].IsChecked = true;
            }

            if (_proxyCombo != null)
            {
                for (int i = 0; i < _proxyCombo.Items.Count; i++)
                {
                    if (string.Equals(Convert.ToString(_proxyCombo.Items[i]), dllName, StringComparison.OrdinalIgnoreCase))
                    {
                        _proxyCombo.SelectedIndex = i;
                        break;
                    }
                }
            }
            UpdatePrimaryButtonState();
            } finally { _selectingProxy = previousSelectionMode; }
        }

        string CurrentSelectedProxy
        {
            get
            {
                if (_proxyPills != null)
                {
                    foreach (var kvp in _proxyPills)
                    {
                        if (kvp.Value.IsChecked == true) return kvp.Key;
                    }
                }
                if (_proxyCombo != null && _proxyCombo.SelectedIndex > 0)
                    return Convert.ToString(_proxyCombo.SelectedItem);
                return "";
            }
        }

        void SetActiveBackend(string backend)
        {
            _activeBackend = backend;
            if (_activeCombo != null)
            {
                for (int i = 0; i < _activeCombo.Items.Count; i++)
                {
                    if (string.Equals(Convert.ToString(_activeCombo.Items[i]), backend, StringComparison.OrdinalIgnoreCase))
                    {
                        _activeCombo.SelectedIndex = i;
                        break;
                    }
                }
            }
            UpdateActiveUI();
            SaveBackendChoice();
        }

        void UpdateActiveUI()
        {
            var isZh = _language == "zh";
            var isLmxxf = string.Equals(_activeBackend, "lmxxf", StringComparison.OrdinalIgnoreCase);
            var isDaniel = string.Equals(_activeBackend, "daniel", StringComparison.OrdinalIgnoreCase);
            var isMochi = string.Equals(_activeBackend, "mochizuki", StringComparison.OrdinalIgnoreCase);

            if (_lmxxfActiveLabel != null)
            {
                _lmxxfActiveLabel.Visibility = isLmxxf ? Visibility.Visible : Visibility.Collapsed;
                _lmxxfActiveLabel.Text = isZh ? "★ 当前主后端" : "★ Active Backend";
            }
            if (_lmxxfActiveBtn != null)
            {
                _lmxxfActiveBtn.Visibility = isLmxxf ? Visibility.Collapsed : Visibility.Visible;
                _lmxxfActiveBtn.Content = isZh ? "设为主后端" : "Set as Active";
                _lmxxfActiveBtn.IsEnabled = _lmxxfCheck != null && _lmxxfCheck.IsChecked == true;
            }

            if (_danielActiveLabel != null)
            {
                _danielActiveLabel.Visibility = isDaniel ? Visibility.Visible : Visibility.Collapsed;
                _danielActiveLabel.Text = isZh ? "★ 当前主后端" : "★ Active Backend";
            }
            if (_danielActiveBtn != null)
            {
                _danielActiveBtn.Visibility = isDaniel ? Visibility.Collapsed : Visibility.Visible;
                _danielActiveBtn.Content = isZh ? "设为主后端" : "Set as Active";
                _danielActiveBtn.IsEnabled = _danielCheck != null && _danielCheck.IsChecked == true;
            }

            if (_mochiActiveLabel != null)
            {
                _mochiActiveLabel.Visibility = isMochi ? Visibility.Visible : Visibility.Collapsed;
                _mochiActiveLabel.Text = isZh ? "★ 当前主后端" : "★ Active Backend";
            }
            if (_mochiActiveBtn != null)
            {
                _mochiActiveBtn.Visibility = isMochi ? Visibility.Collapsed : Visibility.Visible;
                _mochiActiveBtn.Content = isZh ? "设为主后端" : "Set as Active";
                _mochiActiveBtn.IsEnabled = _mochiCheck != null && _mochiCheck.IsChecked == true;
            }
        }

        static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
            catch { }
        }

        void SwitchTab(bool toSettings)
        {
            if (_libraryPage != null) _libraryPage.Visibility = toSettings ? Visibility.Collapsed : Visibility.Visible;
            if (_settingsPage != null) _settingsPage.Visibility = toSettings ? Visibility.Visible : Visibility.Collapsed;
            if (_settingsTab != null)
            {
                _settingsTab.Content = toSettings ? (_language == "zh" ? "返回游戏库" : "Library") : (_language == "zh" ? "设置" : "Settings");
            }
        }

        void ToggleLanguage()
        {
            SetLanguage(_language == "zh" ? "en" : "zh");
        }

        void SetLanguage(string language)
        {
            if (language != "zh" && language != "en") return;
            _language = language;
            SaveSettings();
            ApplyLang();
        }

        internal static string ResolveLanguage(string saved, CultureInfo uiCulture)
        {
            if (string.Equals(saved, "zh", StringComparison.OrdinalIgnoreCase)) return "zh";
            if (string.Equals(saved, "en", StringComparison.OrdinalIgnoreCase)) return "en";
            return uiCulture != null && uiCulture.TwoLetterISOLanguageName == "zh" ? "zh" : "en";
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

            RefreshProfiles();
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
                var roots = new List<string>();
                try
                {
                    var asmLoc = typeof(MainController).Assembly.Location;
                    if (!string.IsNullOrEmpty(asmLoc)) roots.Add(Path.GetDirectoryName(asmLoc));
                }
                catch { }
                if (!string.IsNullOrEmpty(AppDomain.CurrentDomain.BaseDirectory))
                    roots.Add(AppDomain.CurrentDomain.BaseDirectory);

                foreach (var start in roots)
                {
                    var dir = start;
                    for (int depth = 0; depth < 5 && !string.IsNullOrEmpty(dir); depth++)
                    {
                        var candidate = Path.Combine(dir, "VERSION");
                        if (File.Exists(candidate))
                        {
                            var raw = File.ReadAllText(candidate).Trim();
                            if (!string.IsNullOrEmpty(raw))
                            {
                                _productVersion = " " + raw;
                                if (_versionText != null)
                                    _versionText.Text = "OptScaler NR v" + raw.TrimStart('v', 'V');
                            }
                            return;
                        }
                        dir = Path.GetDirectoryName(dir.TrimEnd('\\', '/'));
                    }
                }
            }
            catch { }
        }

        static string GetReasonText(string reasonCode, string fallbackReason, bool isZh)
        {
            if (string.IsNullOrEmpty(reasonCode)) return fallbackReason ?? "";
            if (isZh)
            {
                switch (reasonCode.ToLowerInvariant())
                {
                    case "ready": return "就绪";
                    case "missing-weights": return "权重缺失";
                    case "missing-runtime": return "运行库缺失";
                    case "preparable": return "需准备模型";
                    case "missing-model": return "模型缺失";
                    case "partial": return "部分模型就绪";
                    default: return fallbackReason ?? reasonCode;
                }
            }
            else
            {
                switch (reasonCode.ToLowerInvariant())
                {
                    case "ready": return "Ready";
                    case "missing-weights": return "Missing Weights";
                    case "missing-runtime": return "Missing Runtime";
                    case "preparable": return "Preparation Required";
                    case "missing-model": return "Missing Model";
                    case "partial": return "Some Models Ready";
                    default: return fallbackReason ?? reasonCode;
                }
            }
        }

        void ApplyLang()
        {
            _applyingLanguage = true;
            try
            {
                var isZh = _language == "zh";
                _window.Language = XmlLanguage.GetLanguage(isZh ? "zh-CN" : "en-US");
                if (_window != null)
                    _window.Title = "OptScaler NR" + _productVersion + (isZh ? " - 本软件免费，如付费购买请立即退款！" : " - Free software — refund if paid!");
                if (_refundWarning != null)
                    _refundWarning.Text = isZh ? "本软件免费，如付费购买请立即退款！" : "Free software — refund if paid!";
                if (_pageTitle != null) _pageTitle.Text = isZh ? "游戏库" : "Library";
                if (_langToggle != null) _langToggle.Content = isZh ? "🌐 English" : "🌐 Chinese";
                if (_libraryTab != null) _libraryTab.Content = isZh ? "游戏库" : "Library";
                if (_settingsTab != null)
                    _settingsTab.Content = _settingsPage != null && _settingsPage.Visibility == Visibility.Visible
                        ? L("返回游戏库", "Library") : L("设置", "Settings");
                if (_addButton != null) _addButton.Content = isZh ? "+ 添加游戏" : "+ Add Game";
                if (_openFolderButton != null) _openFolderButton.Content = isZh ? "打开目录" : "Open Folder";
                if (_uninstallButton != null) _uninstallButton.Content = isZh ? "卸载" : "Uninstall";
                if (_removeGameMenuItem != null) _removeGameMenuItem.Header = L("从列表移除", "Remove from list");
                if (_compatButton != null) _compatButton.Content = L("兼容修复", "Compat Repair");
                if (_restoreCompatButton != null) _restoreCompatButton.Content = L("恢复原文件", "Restore Files");
                if (_compatHelpButton != null) _compatHelpButton.ToolTip = L("一般在什么情况下需要使用兼容修复？", "When should you use Compatibility Repair?");
                UpdateCompatibilitySummary();
                if (_openLogButton != null) _openLogButton.Content = isZh ? "打开完整日志" : "Open Full Log";
                if (_refreshButton != null) _refreshButton.Content = isZh ? "刷新" : "Refresh";
                if (_packageBrowse != null) _packageBrowse.Content = isZh ? "浏览…" : "Browse…";
                if (_heroKicker != null) _heroKicker.Text = isZh ? "已选游戏" : "Selected Game";
                if (_replaceIni != null) _replaceIni.Content = isZh ? "覆盖现有 OptiScaler.ini 配置文件 (推荐)" : "Replace existing OptiScaler.ini (recommended)";
                if (_scanOnStartup != null) _scanOnStartup.Content = isZh ? "启动时自动扫描游戏库" : "Scan game library on startup";

                SetLabel("LocalGamesHeading", "本地游戏", "Local Games");
                SetLabel("ProxyHeading", "注入代理 (Proxy DLL)", "Injection Proxy (DLL)");
                SetLabel("BackendsHeading", "后端加速管线", "Backend Engines");
                SetLabel("LmxxfCheck", "lmxxf · 0.44", "lmxxf · 0.44");
                SetLabel("DanielCheck", "danielblnc · 0.3.0-0.6.0", "danielblnc · 0.3.0-0.6.0");
                SetLabel("MochiCheck", "mochizuki0323 · 0.0.7", "mochizuki0323 · 0.0.7");
                SetLabel("LmxxfDescription", "适合 RX 9000 系列", "For RX 9000 series");
                SetLabel("DanielDescription", "适合 RX 6000/7000/9000 系列", "For RX 6000/7000/9000 series");
                SetLabel("MochiDescription", "适合 RX 9000 系列", "For RX 9000 series");
                SetLabel("DiagnosticsHeading", "配置与诊断", "Configuration & Diagnostics");
                SetLabel("LogHeading", "运行诊断日志", "Diagnostic Log");
                SetLabel("SettingsHeading", "设置", "Settings");
                SetLabel("LanguageHeading", "语言", "Language");
                SetLabel("PackageHeading", "安装包目录", "Package Folder");
                SetLabel("PackageDesc",
                    "默认无需填写（自动使用启动器所在目录）；仅在将启动器单独移出完整解压包或源码调试时手动指定。",
                    "Leave blank by default (uses launcher folder); only set when running outside the extracted package.");
                SetLabel("CommunityHeading", "关于与社区", "About & Community");
                SetLabel("GithubButton", "GitHub · 项目主页", "GitHub · Repository");
                SetLabel("BilibiliButton", "Bilibili · 作者主页", "Bilibili · Channel");
                if (_selectedGame == null)
                {
                    SetLabel("HeroTitle", "选择一个游戏以开始", "Select a game to get started");
                    SetLabel("HeroMeta", "选择游戏后自动检查运行状态与注入环境", "Select a game to inspect its installation and proxy options.");
                }
                if (_langCombo != null)
                {
                    ((ComboBoxItem)_langCombo.Items[0]).Content = L("简体中文", "Chinese");
                    ((ComboBoxItem)_langCombo.Items[1]).Content = "English";
                    _langCombo.SelectedIndex = isZh ? 0 : 1;
                }

                if (_filterCombo != null && _filterCombo.Items.Count >= 3)
                {
                    var idx = _filterCombo.SelectedIndex;
                    _filterCombo.Items.Clear();
                    _filterCombo.Items.Add(isZh ? "全部" : "All");
                    _filterCombo.Items.Add(isZh ? "已安装" : "Installed");
                    _filterCombo.Items.Add(isZh ? "未安装" : "Not Installed");
                    _filterCombo.SelectedIndex = idx >= 0 ? idx : 0;
                }

                if (_proxyCombo != null && _proxyCombo.Items.Count > 0)
                {
                    var idx = _proxyCombo.SelectedIndex;
                    _proxyCombo.Items[0] = isZh ? "请选择" : "Select DLL";
                    if (idx >= 0) _proxyCombo.SelectedIndex = idx;
                }

                UpdateActiveUI();
                UpdatePrimaryButtonState();
                RefreshInspectionText();
                RefreshLibraryActions();
                UpdateProxyRecommendation();
                RefreshStatusLanguage();
                RefreshGameListView();
            }
            finally { _applyingLanguage = false; }
        }

        string L(string zh, string en) { return _language == "zh" ? zh : en; }

        void SetLabel(string name, string zh, string en)
        {
            var element = _window.FindName(name);
            var text = element as TextBlock;
            if (text != null) text.Text = L(zh, en);
            var content = element as ContentControl;
            if (content != null) content.Content = L(zh, en);
        }

        void RefreshInspectionText()
        {
            // Refresh presentation only. Reapplying inspection would reset the user's choices.
            var result = _lastInspectResult;
            var failed = result != null && !string.IsNullOrEmpty(result.Status) && result.Status != "success";
            if (_selectedGame != null && !_selectedGame.HasExecutable) {
                if (_injectText != null) _injectText.Text = L("尚未定位启动程序。可以保留文件夹条目，扫描或选择 EXE 后再安装。", "Executable not selected. Keep this folder entry and find or choose its EXE before installing.");
                UpdateProxyRecommendation();
                foreach (var label in new[] { _lmxxfReason, _danielReason, _mochiReason }) if (label != null) { label.Text = L("待选择 EXE", "Select EXE first"); label.Foreground = MissingBrush; }
                foreach (var badge in new[] { _lmxxfBadge, _danielBadge, _mochiBadge }) if (badge != null) { badge.Background = MissingBg; badge.BorderBrush = MissingBorder; }
                RefreshBackendHints();
                return;
            }
            if (_injectText != null) _injectText.Text = failed
                ? L("检查失败，请重新选择游戏以重试。", "Inspection failed. Select the game again to retry.")
                : result != null
                ? L("分析完成，请确认注入代理与后端配置：", "Analysis complete. Confirm the proxy DLL and backends:")
                : (_selectedGame != null
                    ? L("正在检查导入与后端…", "Checking imports and backends…")
                    : L("请从下方选择游戏启动时所加载的拦截代理 DLL：", "Choose the proxy DLL to load when the game starts:"));
            if (_injectRecommendation != null) _injectRecommendation.Text = failed
                ? L("检测未完成：", "Inspection incomplete: ") + result.Error
                : result == null
                ? L("选择游戏后将分析 PE 导入表并自动匹配推荐代理。", "Select a game to inspect its imports and recommend a proxy.")
                : (!string.IsNullOrEmpty(result.Recommend)
                    ? L("导入表候选：" + result.Recommend + "，实际可用性待确认。", "Import candidate: " + result.Recommend + "; compatibility is unverified.")
                    : L("没有足够证据推荐代理，请手动选择。", "Insufficient evidence to recommend a proxy. Choose manually."));
            foreach (var id in new[] { "lmxxf", "daniel", "mochizuki" })
            {
                BackendInfo capability = null;
                if (result != null && result.Backends != null)
                    foreach (var candidate in result.Backends)
                        if (candidate != null && candidate.Id == id) { capability = candidate; break; }
                var ready = capability != null && capability.Enabled;
                var reason = capability != null ? GetReasonText(capability.ReasonCode, capability.Reason, _language == "zh")
                    : failed ? L("检查失败", "Check failed") : L("未检查", "Not checked");
                var label = id == "lmxxf" ? _lmxxfReason : id == "daniel" ? _danielReason : _mochiReason;
                var badge = id == "lmxxf" ? _lmxxfBadge : id == "daniel" ? _danielBadge : _mochiBadge;
                if (label != null)
                {
                    label.Text = reason;
                    label.Foreground = ready ? ReadyBrush : MissingBrush;
                }
                if (badge != null)
                {
                    badge.Background = ready ? ReadyBg : MissingBg;
                    badge.BorderBrush = ready ? ReadyBorder : MissingBorder;
                }
            }
            RefreshBackendHints();
            if (!failed) UpdateProxyRecommendation();
        }

        string BackendHintText(BackendInfo backend)
        {
            var lines = new List<string>();
            if (backend.MissingPackageFiles != null && backend.MissingPackageFiles.Length > 0)
            {
                var files = string.Join(", ", backend.MissingPackageFiles, 0, Math.Min(3, backend.MissingPackageFiles.Length));
                if (backend.MissingPackageFiles.Length > 3) files += L(" 等 " + backend.MissingPackageFiles.Length + " 项", " (" + backend.MissingPackageFiles.Length + " files)");
                lines.Add(L("安装包缺少 " + files + "，请重新解压完整安装包。",
                    "Package file missing: " + files + ". Extract the complete package again."));
            }
            var hintSet = new HashSet<string>(backend.Hints ?? new string[0], StringComparer.OrdinalIgnoreCase);
            if (hintSet.Contains("lmxxf-original") && hintSet.Contains("lmxxf-small"))
            {
                lines.Add(L("缺少模型：请将 nvngx_dlssnr.dll 放到安装包目录（也支持已解压的 native-game-tiled-assets）。",
                    "Models missing: place nvngx_dlssnr.dll beside Setup.bat (or supply native-game-tiled-assets)."));
            }
            else if (hintSet.Contains("lmxxf-original-preparable") && hintSet.Contains("lmxxf-small-preparable"))
            {
                lines.Add(L("已找到模型来源，安装时自动提取。", "Model source found. Selected models will be extracted during installation."));
            }
            foreach (var hint in backend.Hints ?? new string[0])
            {
                switch (hint)
                {
                    case "lmxxf-weights": lines.Add(L("缺少模型：请将 nvngx_dlssnr.dll 放到安装包目录。", "lmxxf weights are missing. Place nvngx_dlssnr.dll beside Setup.bat.")); break;
                    case "lmxxf-original":
                        if (!hintSet.Contains("lmxxf-small"))
                            lines.Add(L("Original 未安装：放入 nvngx_dlssnr.dll 即可提取。", "Original not installed: place nvngx_dlssnr.dll beside Setup.bat to extract."));
                        break;
                    case "lmxxf-original-preparable":
                        if (!hintSet.Contains("lmxxf-small-preparable"))
                            lines.Add(L("Original 未安装：选择后将自动提取。", "Original not installed: will be extracted if selected."));
                        break;
                    case "lmxxf-small":
                        if (!hintSet.Contains("lmxxf-original"))
                            lines.Add(L("Small 未安装：需要 nvngx_dlssnr.dll (310.9.16)。", "Small not installed: requires nvngx_dlssnr.dll (310.9.16)."));
                        break;
                    case "lmxxf-small-preparable":
                        if (!hintSet.Contains("lmxxf-original-preparable"))
                            lines.Add(L("Small 未安装：选择后将自动提取。", "Small not installed: will be extracted if selected."));
                        break;
                    case "mochi-source": lines.Add(L("缺少 Mochi 模型：请将 nvngx_dlssnr.dll 放到安装包目录。", "Mochi models are missing. Place nvngx_dlssnr.dll beside Setup.bat.")); break;
                    case "mochi-preparable": lines.Add(L("已找到模型来源，安装时自动提取。", "Model source found. Models will be prepared during installation.")); break;
                    case "mochi-versions": lines.Add(L("310.9.16 支持新旧模型；310.8.0 仅支持原始模型。", "310.9.16: both models. 310.8.0: original only.")); break;
                    case "mochi-unsupported-source": lines.Add(L("此 nvngx_dlssnr.dll 版本不受支持，请更换为 310.9.16 或 310.8.0。", "Unsupported nvngx_dlssnr.dll. Supply 310.9.16 or 310.8.0.")); break;
                    case "daniel-runtime": lines.Add(L("缺少 Daniel 运行库：请放入 Daniel 的 dlssnr_on_amd_setup.exe 或 version.dll。", "Daniel runtime missing: place Daniel's dlssnr_on_amd_setup.exe or Daniel's version.dll beside Setup.bat.")); break;
                    case "daniel-weights": lines.Add(L("缺少 Daniel 权重：请放入 nvngx_dlssnr.dll（或 dlssnr_on_amd_weights.bin）。", "Daniel weights missing: place nvngx_dlssnr.dll (or dlssnr_on_amd_weights.bin) beside Setup.bat.")); break;
                    case "daniel-setup":
                        if (!hintSet.Contains("daniel-runtime"))
                            lines.Add(L("请放入 Daniel 的 dlssnr_on_amd_setup.exe 或 version.dll。", "Place Daniel's dlssnr_on_amd_setup.exe or Daniel's version.dll beside Setup.bat."));
                        break;
                    case "daniel-source":
                        if (!hintSet.Contains("daniel-weights"))
                            lines.Add(L("请将 nvngx_dlssnr.dll 放到安装包目录。", "Place nvngx_dlssnr.dll beside Setup.bat."));
                        break;
                    case "daniel-preparable": lines.Add(L("已找到提取文件，安装时自动提取。", "Extraction files found. Preparation will run during installation.")); break;
                }
            }
            return string.Join(Environment.NewLine, lines.ToArray());
        }

        void RefreshBackendHints()
        {
            bool any = false;
            foreach (var id in new[] { "mochizuki", "lmxxf", "daniel" })
            {
                BackendInfo backend = null;
                if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                    foreach (var item in _lastInspectResult.Backends)
                        if (item != null && item.Id == id) { backend = item; break; }
                var text = backend != null ? BackendHintText(backend) : "";
                var label = _window.FindName(id == "mochizuki" ? "MochiHint" : id == "lmxxf" ? "LmxxfHint" : "DanielHint") as TextBlock;
                if (label != null)
                {
                    label.Text = text;
                    label.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
                    label.ToolTip = backend != null && backend.MissingPackageFiles != null && backend.MissingPackageFiles.Length > 3
                        ? string.Join(Environment.NewLine, backend.MissingPackageFiles) : null;
                }
                any |= !string.IsNullOrEmpty(text);
            }
            var path = _lastInspectResult != null ? _lastInspectResult.PackageRoot : null;
            var pathLabel = _window.FindName("BackendPackagePath") as TextBlock;
            if (pathLabel != null)
            {
                pathLabel.Text = L("安装包目录：", "Package folder: ") + (path ?? "");
                pathLabel.Visibility = any && !string.IsNullOrEmpty(path) ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        void RefreshStatusLanguage()
        {
            if (_statusText == null) return;
            if (_session != null && _session.IsRunning)
            {
                _statusText.Text = StageText(_eventStage ?? _session.Action);
                return;
            }
            switch (_statusText.Text)
            {
                case "就绪": case "Ready": _statusText.Text = L("就绪", "Ready"); break;
                case "正在扫描游戏库…": case "Scanning library…": _statusText.Text = L("正在扫描游戏库…", "Scanning library…"); break;
                case "游戏库已更新": case "Library updated": _statusText.Text = L("游戏库已更新", "Library updated"); break;
                case "Cancelled.": case "已取消。": _statusText.Text = L("已取消。", "Cancelled."); break;
                case "success": case "成功": _statusText.Text = L("成功", "success"); break;
                case "failed": case "失败": _statusText.Text = L("失败", "failed"); break;
                case "正在检查安装环境…": case "Inspecting installation…": _statusText.Text = StageText("inspect"); break;
                case "正在检查安装条件…": case "Validating installation…": _statusText.Text = StageText("validate"); break;
                case "正在准备模型…": case "Preparing models…": _statusText.Text = StageText("prepare-model"); break;
                case "正在保存备份…": case "Saving backups…": _statusText.Text = StageText("backup"); break;
                case "正在验证安装结果…": case "Verifying installation…": _statusText.Text = StageText("verify"); break;
                case "操作已结束": case "Operation finished": _statusText.Text = StageText("done"); break;
                case "正在应用更改…": case "Applying changes…": _statusText.Text = StageText("install"); break;
                case "目标文件夹已被另一个安装进程锁定。": case "Target folder is locked by another launcher.":
                    _statusText.Text = L("目标文件夹已被另一个安装进程锁定。", "Target folder is locked by another launcher."); break;
            }
        }

        string StageText(string stage)
        {
            switch (stage)
            {
                case "scan": return L("正在扫描游戏库…", "Scanning library…");
                case "inspect": return L("正在检查安装环境…", "Inspecting installation…");
                case "validate": return L("正在检查安装条件…", "Validating installation…");
                case "prepare-model": return L("正在准备模型…", "Preparing models…");
                case "backup": return L("正在保存备份…", "Saving backups…");
                case "verify": return L("正在验证安装结果…", "Verifying installation…");
                case "done": return L("操作已结束", "Operation finished");
                default: return L("正在应用更改…", "Applying changes…");
            }
        }

        void LoadSettings()
        {
            GuiSettings rec = null;
            try
            {
                if (File.Exists(_settingsPath)) rec = JsonUtil.Read<GuiSettings>(File.ReadAllText(_settingsPath));
            }
            catch { }
            // CurrentUICulture uses the Windows display language, independently of number/date formats.
            // Only an absent/unsupported preference consults it; the resolved choice is saved once.
            var hasSavedLanguage = rec != null && (string.Equals(rec.Language, "zh", StringComparison.OrdinalIgnoreCase)
                || string.Equals(rec.Language, "en", StringComparison.OrdinalIgnoreCase));
            _saveInitialLanguage = !hasSavedLanguage;
            _language = hasSavedLanguage ? rec.Language.ToLowerInvariant() : ResolveLanguage(null, CultureInfo.CurrentUICulture);
            if (rec != null)
            {
                _recentGameId = rec.RecentGameId;
                if (rec.ProxyChoices != null)
                    foreach (var pair in rec.ProxyChoices) if (ProxyRecommendations.Supports(pair.Value)) _proxyChoices[pair.Key] = pair.Value;
                if (rec.HiddenGames != null)
                    foreach (var pair in rec.HiddenGames) _hiddenGames[pair.Key] = pair.Value;
                if (rec.BackendChoices != null)
                    foreach (var pair in rec.BackendChoices)
                        if (pair.Value != null && pair.Value.Backends != null)
                            _backendChoices[pair.Key] = pair.Value;
                _isScanOnStartup = rec.ScanOnStartup;
                if (_scanOnStartup != null) _scanOnStartup.IsChecked = _isScanOnStartup;
                if (!string.IsNullOrEmpty(rec.PackageRoot))
                {
                    _packageRoot = rec.PackageRoot;
                    if (_packageRootBox != null) _packageRootBox.Text = _packageRoot;
                }
            }
        }

        void SaveSettings()
        {
            if (_loadingSettings) return;
            try
            {
                Directory.CreateDirectory(_settingsDir);
                var set = new GuiSettings
                {
                    Language = _language,
                    ScanOnStartup = _isScanOnStartup,
                    PackageRoot = _packageRoot,
                    RecentGameId = _selectedGame != null ? _selectedGame.Id : _recentGameId,
                    BackendChoices = _backendChoices,
                    HiddenGames = _hiddenGames,
                    ProxyChoices = _proxyChoices
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
                    bool migrated = false;
                    foreach (var g in rec.Games)
                    {
                        if (g != null && (!string.IsNullOrEmpty(g.Exe) || !string.IsNullOrEmpty(g.Root)) && !IsHiddenGame(g) &&
                            (g.ExeOverride == true || g.Source == "Manual" || g.HasExecutable))
                        {
                            if (string.IsNullOrEmpty(g.Id)) { g.Id = "manual:" + Guid.NewGuid().ToString("N"); migrated = true; }
                            if (string.IsNullOrEmpty(g.Root) && !string.IsNullOrEmpty(g.Exe)) { g.Root = Path.GetDirectoryName(g.Exe); migrated = true; }
                            _allGames.Add(g);
                        }
                    }
                    var count = _allGames.Count;
                    MergeScannedGames(new GameInfo[0]);
                    if (_allGames.Count != count || migrated) SaveLibraryCache();
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
            [System.Runtime.Serialization.DataMember] public Dictionary<string, string> ProxyChoices;
            [System.Runtime.Serialization.DataMember] public Dictionary<string, string> HiddenGames;
            [System.Runtime.Serialization.DataMember] public string Language;
            [System.Runtime.Serialization.DataMember] public bool ScanOnStartup = true;
            [System.Runtime.Serialization.DataMember] public string PackageRoot;
            [System.Runtime.Serialization.DataMember] public string RecentGameId;
            [System.Runtime.Serialization.DataMember] public Dictionary<string, BackendChoice> BackendChoices;
        }

        [System.Runtime.Serialization.DataContract]
        sealed class BackendChoice
        {
            [System.Runtime.Serialization.DataMember] public string[] Backends;
            [System.Runtime.Serialization.DataMember] public string ActiveBackend;
        }

        string BackendChoiceKey()
        {
            return _selectedGame == null ? null : (!string.IsNullOrEmpty(_selectedGame.Id) ? _selectedGame.Id : _selectedGame.Exe);
        }

        BackendChoice CurrentBackendChoice()
        {
            BackendChoice choice;
            var key = BackendChoiceKey();
            return !string.IsNullOrEmpty(key) && _backendChoices.TryGetValue(key, out choice) ? choice : null;
        }

        List<string> CheckedBackends()
        {
            var list = new List<string>();
            if (_mochiCheck != null && _mochiCheck.IsChecked == true) list.Add("mochizuki");
            if (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true) list.Add("lmxxf");
            if (_danielCheck != null && _danielCheck.IsChecked == true) list.Add("daniel");
            return list;
        }

        void SaveBackendChoice()
        {
            var key = BackendChoiceKey();
            if (_loadingSettings || _updatingBackends || string.IsNullOrEmpty(key)) return;
            var list = CheckedBackends();
            _backendChoices[key] = new BackendChoice { Backends = list.ToArray(), ActiveBackend = list.Contains(_activeBackend) ? _activeBackend : "" };
            SaveSettings();
        }

        void RestoreBackendChoice()
        {
            var choice = CurrentBackendChoice();
            var list = new List<string>(choice != null ? choice.Backends : new string[0]);
            _updatingBackends = true;
            try
            {
                if (_mochiCheck != null) _mochiCheck.IsChecked = list.Contains("mochizuki");
                if (_lmxxfCheck != null) _lmxxfCheck.IsChecked = list.Contains("lmxxf");
                if (_danielCheck != null) _danielCheck.IsChecked = list.Contains("daniel");
                _activeBackend = choice != null ? choice.ActiveBackend : "mochizuki";
                UpdateActiveCombo();
            }
            finally { _updatingBackends = false; }
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
                throw new FileNotFoundException(L("缺少 launcher-worker.ps1，请重新解压完整安装包。", "Complete package required: launcher-worker.ps1 is missing."));
            }
        }

        void BrowsePackageRoot()
        {
            var dlg = new OpenFileDialog
            {
                Title = L("选择完整安装包目录（选择 Setup.ps1 或 OptiScaler.dll）", "Select package folder (pick Setup.ps1 or OptiScaler.dll)"),
                Filter = L("安装包文件 (Setup.ps1;OptiScaler.dll)|Setup.ps1;OptiScaler.dll|所有文件 (*.*)|*.*", "Package files (Setup.ps1;OptiScaler.dll)|Setup.ps1;OptiScaler.dll|All files (*.*)|*.*"),
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
            var picked = GamePaths.PickExe(_language == "zh");
            if (string.IsNullOrEmpty(picked)) return;
            AddGameFromPath(picked);
        }

        void AddGameFromPath(string exePath)
        {
            var norm = GamePaths.Normalize(exePath);
            if (string.IsNullOrEmpty(norm) || !GamePaths.CanUse(norm)) return;
            var restored = new List<string>();
            foreach (var pair in _hiddenGames)
                if (string.Equals(pair.Value, norm, StringComparison.OrdinalIgnoreCase)) restored.Add(pair.Key);
            foreach (var id in restored) _hiddenGames.Remove(id);
            SaveSettings();

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
                foreach (var candidate in _allGames)
                    if (!candidate.HasExecutable && !string.IsNullOrEmpty(candidate.RootDirectory) &&
                        norm.StartsWith(candidate.RootDirectory.TrimEnd('\\','/') + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
                        (found == null || candidate.RootDirectory.Length > found.RootDirectory.Length)) found = candidate;
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
                    Id = "manual:" + Guid.NewGuid().ToString("N"),
                    Name = name,
                    Exe = norm,
                    Root = dir,
                    Source = "Manual",
                    Installed = File.Exists(Path.Combine(dir, "amd-presr-install.txt"))
                };
                _allGames.Add(found);
            }

            // Picking an existing scanned entry is also an explicit EXE choice.
            found.Exe = norm;
            found.ExeOverride = true;
            found.Installed = File.Exists(Path.Combine(Path.GetDirectoryName(norm), "amd-presr-install.txt"));
            SaveLibraryCache();
            RefreshGameListView();
            SelectGame(found);
        }

        void ChangeExe()
        {
            if (_selectedGame == null) return;
            var picked = GamePaths.PickExe(_language == "zh", _selectedGame.HasExecutable ? Path.GetDirectoryName(_selectedGame.Exe) : _selectedGame.RootDirectory);
            if (string.IsNullOrEmpty(picked)) return;
            var norm = GamePaths.Normalize(picked);
            if (string.IsNullOrEmpty(norm) || !GamePaths.CanUse(norm)) return;

            BindExecutable(_selectedGame, norm);
        }

        bool IsHiddenGame(GameInfo game)
        {
            if (!string.IsNullOrEmpty(game.Id) && _hiddenGames.ContainsKey(game.Id)) return true;
            foreach (var path in _hiddenGames.Values)
                if (SamePath(path, EntryLocation(game))) return true;
            return false;
        }

        void InitGameContextMenu()
        {
            if (_gameList == null) return;
            var menu = new ContextMenu();
            menu.Style = (Style)_window.FindResource("GameContextMenu");
            _removeGameMenuItem = new MenuItem { Header = L("从列表移除", "Remove from list"), IsEnabled = false };
            _removeGameMenuItem.Style = (Style)_window.FindResource("GameContextMenuItem");
            _removeGameMenuItem.Click += (s, e) => RemoveGame(_removeGameMenuItem.Tag as GameInfo);
            menu.Items.Add(_removeGameMenuItem);
            _gameList.ContextMenu = menu;
            // Right-click targets a row without starting its automatic inspection.
            _gameList.PreviewMouseRightButtonDown += (s, e) =>
            {
                if (ItemsControl.ContainerFromElement(_gameList, e.OriginalSource as DependencyObject) is ListBoxItem)
                    e.Handled = true;
            };
            _gameList.ContextMenuOpening += (s, e) =>
            {
                var row = ItemsControl.ContainerFromElement(_gameList, e.OriginalSource as DependencyObject) as ListBoxItem;
                var item = row == null ? null : row.Content as GameItemViewModel;
                if (item == null && e.CursorLeft < 0) item = _gameList.SelectedItem as GameItemViewModel;
                if (item == null || item.Info == null) { e.Handled = true; return; }
                _removeGameMenuItem.Tag = item.Info;
                _removeGameMenuItem.Header = L("从列表移除", "Remove from list");
                _removeGameMenuItem.IsEnabled = !_writeBusy && (_session == null || !_session.IsRunning);
            };
        }

        void RemoveGame(GameInfo removed)
        {
            if (removed == null || _writeBusy || (_session != null && _session.IsRunning)) return;
            var wasSelected = _selectedGame != null && SameLibraryGame(_selectedGame, removed);
            _hiddenGames[removed.Id ?? ("custom:" + EntryLocation(removed).ToLowerInvariant())] = EntryLocation(removed);
            _allGames.RemoveAll(g => SameLibraryGame(g, removed));
            if (wasSelected) SelectGame(null);
            SaveSettings();
            SaveLibraryCache();
            RefreshGameListView();
            if (wasSelected && _gameList != null && _gameList.Items.Count > 0) _gameList.SelectedIndex = 0;
            if (_statusText != null) _statusText.Text = L("已从列表移除；游戏文件未改动。可通过“添加游戏”重新添加。", "Removed from the list; game files were kept. Use Add Game to add it again.");
        }

        void OpenSelectedFolder()
        {
            if (_selectedGame == null) return;
            try
            {
                var dir = _selectedGame.RootDirectory;
                if (Directory.Exists(dir))
                {
                    Process.Start("explorer.exe", dir);
                }
            }
            catch { }
        }

        void OnDrop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files == null || files.Length == 0) return;
            var path = files[0];
            if (Directory.Exists(path))
            {
                AddFolderFromPath(path);
                return;
            }
            if (path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                AddGameFromPath(path);
            }
        }

        static ImageSource GetOrExtractIcon(GameInfo g)
        {
            if (g == null || string.IsNullOrEmpty(g.Exe)) return null;
            if (!string.IsNullOrEmpty(g.DisplayCover) && File.Exists(g.DisplayCover)) return null;
            try
            {
                if (File.Exists(g.Exe))
                {
                    using (var ico = System.Drawing.Icon.ExtractAssociatedIcon(g.Exe))
                    {
                        if (ico != null)
                        {
                            return Imaging.CreateBitmapSourceFromHIcon(
                                ico.Handle,
                                Int32Rect.Empty,
                                BitmapSizeOptions.FromEmptyOptions());
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        void RefreshGameListView()
        {
            var query = (_searchBox != null ? _searchBox.Text : "").Trim().ToLowerInvariant();
            var filterIdx = _filterCombo != null ? _filterCombo.SelectedIndex : 0;
            var isZh = _language == "zh";

            var list = new List<GameItemViewModel>();
            foreach (var g in _allGames)
            {
                if (filterIdx == 1 && !g.Installed) continue;
                if (filterIdx == 2 && g.Installed) continue;
                if (!string.IsNullOrEmpty(query))
                {
                    var matchName = (g.DisplayName ?? "").ToLowerInvariant().Contains(query) || (g.Name ?? "").ToLowerInvariant().Contains(query);
                    var matchExe = g.Exe != null && g.Exe.ToLowerInvariant().Contains(query);
                    if (!matchName && !matchExe) continue;
                }
                var icon = GetOrExtractIcon(g);
                list.Add(new GameItemViewModel { Info = g, IsZh = isZh, IconSource = icon });
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

            if (_countBadge != null) _countBadge.Text = list.Count.ToString();
            if (_countText != null)
            {
                _countText.Text = isZh
                    ? string.Format("共 {0} 款游戏", list.Count)
                    : string.Format("{0} games", list.Count);
            }
        }

        void OnGameSelected()
        {
            if (_gameList == null) return;
            var item = _gameList.SelectedItem as GameItemViewModel;
            if (item != null && item.Info != null)
            {
                if (_selectedGame != null && string.Equals(_selectedGame.Id, item.Info.Id, StringComparison.OrdinalIgnoreCase))
                    return;
                SelectGame(item.Info);
            }
        }

        void SelectGame(GameInfo game)
        {
            if (_writeBusy) return;
            if (_session != null && _session.CanKill && _session.Action == "inspect") CancelReadSession();
            if (_folderScan != null) _folderScan.Cancel();
            SelectProxy(null);
            _decisions.ForeignProxy.Clear();
            _resumeInstallAction = "";
            if (game == null || _selectedGame == null || !SameLibraryGame(game, _selectedGame)) _decisions.ModelSourceDll = null;
            _selectedGame = game;
            _lastInspectResult = null;
            if (game == null) {
                _recentGameId = null;
                if (_heroTitle != null) _heroTitle.Text = L("选择一个游戏以开始", "Select a game to begin");
                if (_heroMeta != null) _heroMeta.Text = "";
                if (_heroPath != null) _heroPath.Text = "";
                if (_heroPosterImage != null) _heroPosterImage.Source = null;
                if (_heroBgImage != null) _heroBgImage.Source = null;
                RefreshInspectionText();
                UpdatePrimaryButtonState();
                return;
            }
            RestoreBackendChoice();
            RefreshInspectionText();
            if (_heroTitle != null) _heroTitle.Text = game.DisplayName;
            if (_heroMeta != null) _heroMeta.Text = game.Source + " · PC";
            if (_heroPath != null) _heroPath.Text = EntryLocation(game);

            if (_heroBgImage != null)
            {
                if (!string.IsNullOrEmpty(game.DisplayHero) && File.Exists(game.DisplayHero))
                {
                    try
                    {
                        var bi = new BitmapImage();
                        bi.BeginInit();
                        bi.CacheOption = BitmapCacheOption.OnLoad;
                        bi.UriSource = new Uri(game.DisplayHero, UriKind.Absolute);
                        bi.EndInit();
                        _heroBgImage.Source = bi;
                    }
                    catch { _heroBgImage.Source = null; }
                }
                else
                {
                    _heroBgImage.Source = null;
                }
            }

            if (_heroPosterImage != null && _heroPosterInitial != null)
            {
                bool loaded = false;
                if (!string.IsNullOrEmpty(game.DisplayCover) && File.Exists(game.DisplayCover))
                {
                    try
                    {
                        var bi = new BitmapImage();
                        bi.BeginInit();
                        bi.CacheOption = BitmapCacheOption.OnLoad;
                        bi.DecodePixelWidth = 180;
                        bi.UriSource = new Uri(game.DisplayCover, UriKind.Absolute);
                        bi.EndInit();
                        _heroPosterImage.Source = bi;
                        _heroPosterImage.Visibility = Visibility.Visible;
                        _heroPosterInitial.Visibility = Visibility.Collapsed;
                        loaded = true;
                    }
                    catch { }
                }

                if (!loaded)
                {
                    var icon = GetOrExtractIcon(game);
                    if (icon != null)
                    {
                        _heroPosterImage.Source = icon;
                        _heroPosterImage.Visibility = Visibility.Visible;
                        _heroPosterInitial.Visibility = Visibility.Collapsed;
                        loaded = true;
                    }
                }

                if (!loaded)
                {
                    _heroPosterImage.Source = null;
                    _heroPosterImage.Visibility = Visibility.Collapsed;
                    _heroPosterInitial.Text = string.IsNullOrEmpty(game.DisplayName) ? "?" : game.DisplayName.Substring(0, 1).ToUpperInvariant();
                    _heroPosterInitial.Visibility = Visibility.Visible;
                }
            }

            UpdatePrimaryButtonState();
            InspectGame(game);
        }

        void UpdatePrimaryButtonState()
        {
            UpdateCompatibilitySummary();
            RefreshLibraryActions();
            if (_removeGameMenuItem != null) _removeGameMenuItem.IsEnabled = _removeGameMenuItem.Tag is GameInfo && !_writeBusy && (_session == null || !_session.IsRunning);
            if (_primaryButton == null) return;
            var isZh = _language == "zh";
            if (_selectedGame == null)
            {
                _primaryButton.Content = isZh ? "⚡ 选择一个游戏" : "⚡ Select a Game";
                _primaryButton.IsEnabled = false;
                if (_uninstallButton != null) _uninstallButton.IsEnabled = false;
                return;
            }

            var hasBackend = (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true)
                || (_danielCheck != null && _danielCheck.IsChecked == true)
                || (_mochiCheck != null && _mochiCheck.IsChecked == true);

            var proxyChosen = !string.IsNullOrEmpty(CurrentSelectedProxy);
            var isBusy = _session != null && _session.IsRunning;
            if (!_selectedGame.HasExecutable) {
                _primaryButton.Content = L("扫描 / 选择启动程序", "Find / choose game EXE");
                _primaryButton.IsEnabled = !isBusy && !_writeBusy && _folderScan == null;
                if (_uninstallButton != null) _uninstallButton.IsEnabled = false;
                return;
            }

            _primaryButton.Content = _selectedGame.Installed
                ? (isZh ? "⚡ 更新 OptScaler NR" : "⚡ Update OptScaler NR")
                : (isZh ? "⚡ 安装 OptScaler NR" : "⚡ Install OptScaler NR");
            _primaryButton.IsEnabled = !isBusy && !_writeBusy && _folderScan == null && hasBackend && proxyChosen;
            if (_uninstallButton != null)
                _uninstallButton.IsEnabled = !isBusy && _selectedGame.Installed;
        }

        void UpdateActiveCombo()
        {
            var checkedBackends = CheckedBackends();
            if (checkedBackends.Count == 0) _activeBackend = "";

            if (_activeCombo != null)
            {
                _activeCombo.Items.Clear();
                foreach (var b in checkedBackends) _activeCombo.Items.Add(b);

                if (checkedBackends.Count >= 1)
                {
                    if (checkedBackends.Contains(_activeBackend))
                        _activeCombo.SelectedItem = _activeBackend;
                    else
                    {
                        _activeBackend = checkedBackends[0];
                        _activeCombo.SelectedIndex = 0;
                    }
                }
            }

            if (!checkedBackends.Contains(_activeBackend) && checkedBackends.Count > 0)
            {
                _activeBackend = checkedBackends[0];
            }

            UpdateActiveUI();
        }

        void RunScan()
        {
            if (_session != null && _session.IsRunning) return;
            SetBusy(true, "scan");
            if (_statusText != null) _statusText.Text = _language == "zh" ? "正在扫描游戏库…" : "Scanning library…";
            if (_statusLed != null) _statusLed.Fill = WarningBrush;
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
                if (ex.Data.Contains("logPath")) _currentLogPath = Convert.ToString(ex.Data["logPath"]);
                AppendLog("Scan error: " + ex.Message);
            }
        }

        static bool SameLibraryGame(GameInfo a, GameInfo b)
        {
            return (!string.IsNullOrEmpty(a.Id) && !string.IsNullOrEmpty(b.Id) &&
                    string.Equals(a.Id, b.Id, StringComparison.OrdinalIgnoreCase)) ||
                   SamePath(a.Exe, b.Exe) ||
                   ((string.IsNullOrEmpty(a.Exe) || string.IsNullOrEmpty(b.Exe)) && SamePath(a.RootDirectory, b.RootDirectory));
        }

        static bool IsAuxiliaryExe(string path)
        {
            var name = Path.GetFileName(path);
            return string.Equals(name, "gamelaunchhelper.exe", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(name, "person-worker.exe", StringComparison.OrdinalIgnoreCase) ||
                   FolderGameScanner.Auxiliary(path) ||
                   path.IndexOf("\\person-model\\", StringComparison.OrdinalIgnoreCase) >= 0 ||
                   path.IndexOf("\\OptiScaler\\", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        static bool KeepCachedExe(GameInfo game)
        {
            if (game.ExeOverride == true || game.Source == "Manual") return true;
            // Legacy caches have no provenance. Correct clear helper/missing paths,
            // but preserve a plausible existing EXE that could have been chosen by hand.
            return !game.ExeOverride.HasValue && GamePaths.CanUse(game.Exe) && !IsAuxiliaryExe(game.Exe);
        }

        bool MergeScannedGames(GameInfo[] scanned)
        {
            var previousSelection = _selectedGame;
            var previousExe = previousSelection != null ? previousSelection.Exe : null;
            var previousName = previousSelection != null ? previousSelection.Name : null;
            var merged = new List<GameInfo>();
            foreach (var cached in _allGames)
            {
                if (cached == null || (string.IsNullOrEmpty(cached.Exe) && string.IsNullOrEmpty(cached.Root)) || IsHiddenGame(cached)) continue;
                var index = merged.FindIndex(g => SameLibraryGame(g, cached));
                if (index < 0) merged.Add(cached);
                else
                {
                    var existing = merged[index];
                    if (string.IsNullOrEmpty(existing.CustomName)) existing.CustomName = cached.CustomName;
                    if (string.IsNullOrEmpty(existing.CustomCover)) existing.CustomCover = cached.CustomCover;
                    var explicitChoice = cached.ExeOverride == true || cached.Source == "Manual";
                    var existingExplicit = existing.ExeOverride == true || existing.Source == "Manual";
                    if ((explicitChoice && !existingExplicit) ||
                        (explicitChoice == existingExplicit && KeepCachedExe(cached) && !KeepCachedExe(existing)))
                    {
                        if (string.IsNullOrEmpty(cached.CustomName)) cached.CustomName = existing.CustomName;
                        if (string.IsNullOrEmpty(cached.CustomCover)) cached.CustomCover = existing.CustomCover;
                        merged[index] = cached;
                    }
                }
            }
            foreach (var incoming in scanned)
            {
                if (incoming == null || (string.IsNullOrEmpty(incoming.Exe) && string.IsNullOrEmpty(incoming.Root)) || IsHiddenGame(incoming)) continue;
                var existing = merged.Find(g => SameLibraryGame(g, incoming));
                if (existing == null)
                {
                    incoming.ExeOverride = false;
                    merged.Add(incoming);
                    continue;
                }
                var samePath = string.Equals(existing.Exe, incoming.Exe, StringComparison.OrdinalIgnoreCase);
                if (!KeepCachedExe(existing) || (!existing.HasExecutable && incoming.HasExecutable))
                {
                    existing.Exe = incoming.Exe;
                    existing.ExeOverride = false;
                    samePath = true;
                }
                existing.Installed = samePath ? incoming.Installed : existing.HasExecutable &&
                    File.Exists(Path.Combine(Path.GetDirectoryName(existing.Exe), "amd-presr-install.txt"));
                if (existing.Source != "Manual" && string.Equals(existing.Id, incoming.Id, StringComparison.OrdinalIgnoreCase))
                {
                    existing.Name = incoming.Name;
                    existing.Source = incoming.Source;
                }
                if (!string.IsNullOrEmpty(incoming.Cover)) existing.Cover = incoming.Cover;
                if (!string.IsNullOrEmpty(incoming.Hero)) existing.Hero = incoming.Hero;
            }
            _allGames.Clear();
            _allGames.AddRange(merged);
            if (previousSelection != null)
                _selectedGame = merged.Find(g => SameLibraryGame(g, previousSelection)) ?? previousSelection;
            return previousSelection != null &&
                (!ReferenceEquals(previousSelection, _selectedGame) ||
                 !string.Equals(previousExe, _selectedGame.Exe, StringComparison.OrdinalIgnoreCase) ||
                 previousName != _selectedGame.Name);
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
                if (_progress != null) _progress.IsIndeterminate = false;

                var selectionChanged = false;
                if (result != null && result.Games != null)
                {
                    selectionChanged = MergeScannedGames(result.Games);
                    SaveLibraryCache();
                    RefreshGameListView();
                }

                if (_statusLed != null) _statusLed.Fill = ReadyBrush;
                if (_statusText != null)
                    _statusText.Text = _language == "zh" ? "游戏库已更新" : "Library updated";

                if (selectionChanged) SelectGame(_selectedGame);
                else if (_selectedGame == null && _allGames.Count > 0 && _gameList != null && _gameList.Items.Count > 0)
                {
                    _gameList.SelectedIndex = 0;
                }
            }));
        }

        void CancelReadSession()
        {
            var old = _session;
            _session = null; // Invalidate even a completion already queued on the dispatcher.
            _pendingAction = "";
            old.CancelInspect();
            Detach(old);
            SetBusy(false, "");
            if (_progress != null) _progress.IsIndeterminate = false;
        }

        void InspectGame(GameInfo game)
        {
            if (_writeBusy) return;
            if (_session != null)
            {
                if (_session.CanKill) CancelReadSession();
                else return;
            }
            if (game == null || !game.HasExecutable) { RefreshInspectionText(); return; }

            _lastInspectResult = null;
            var fastProxy = PeProbe.QuickRecommend(game.Exe);
            UpdateProxyRecommendation(fastProxy);

            SetBusy(true, "inspect");
            _pendingAction = "inspect";
            if (_injectText != null) _injectText.Text = _language == "zh" ? "正在检查导入与后端…" : "Checking imports and backends…";
            if (_statusLed != null) _statusLed.Fill = WarningBrush;
            if (_progress != null) _progress.IsIndeterminate = true;
            UpdatePrimaryButtonState();

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = "inspect",
                GameExe = game.Exe,
                ProxyChoice = "auto",
                BackendChoice = "auto",
                ExplicitDecisions = _decisions,
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
                if (ex.Data.Contains("logPath")) _currentLogPath = Convert.ToString(ex.Data["logPath"]);
                AppendLog("Inspect failed: " + ex.Message);
            }
        }

        void OnPrimaryAction()
        {
            if (_selectedGame == null) return;
            if (!_selectedGame.HasExecutable) { ScanExecutables(); return; }
            var action = _selectedGame.Installed ? "update" : "install";
            ExecuteAction(action);
        }

        void OnUninstall()
        {
            if (_selectedGame == null || !_selectedGame.HasExecutable) return;
            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            var isZh = _language == "zh";
            var msg = isZh
                ? ("确定要从以下目录卸载 OptScaler NR 吗？\n" + dir + "\n\n仅删除属于本项目的组件与代理文件。游戏原有文件和第三方插件会被完整保留。")
                : ("Uninstall OptScaler NR from:\n" + dir + "\n\nOwned files are removed. Unowned plugins and the original game files stay.");

            var confirm = DialogTheme.Show(_window, msg, _window.Title, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (confirm != MessageBoxResult.OK) return;

            ExecuteAction("uninstall");
        }

        string XessGroupLabel(string id)
        {
            switch (id)
            {
                case "fg": return L("XeSS 插帧 + XeLL（配套替换）", "XeSS frame generation + XeLL (replace together)");
                case "dx11": return L("XeSS 超分（DX11）", "XeSS upscaling (DX11)");
                case "sr": return L("XeSS 超分（DX12 / Vulkan）", "XeSS upscaling (DX12 / Vulkan)");
                case "fsr_fg": return L("AMD FidelityFX 帧生成（DX12）", "AMD FidelityFX frame generation (DX12)");
                case "fsr_upscaler": return L("AMD FidelityFX 超分（DX12）", "AMD FidelityFX upscaler (DX12)");
                case "fsr_loader": return L("AMD FidelityFX 加载器（DX12）", "AMD FidelityFX loader (DX12)");
                case "fsr_vk": return L("AMD FidelityFX 运行库（Vulkan）", "AMD FidelityFX runtime (Vulkan)");
                default: return id;
            }
        }

        Window CreateCompatibilityHelpDialog()
        {
            var title = L("关于兼容修复与配套库", "About Compatibility Repair & Companion Libraries");
            var dialog = DialogTheme.Create(_window, title, 620);
            var panel = new StackPanel { Margin = new Thickness(22) };

            var statusBox = new Border
            {
                Background = new SolidColorBrush(Color.FromRgb(24, 29, 40)),
                BorderBrush = new SolidColorBrush(Color.FromRgb(40, 48, 66)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(14, 10, 14, 10),
                Margin = new Thickness(0, 0, 0, 16)
            };
            var statusStack = new StackPanel();
            statusStack.Children.Add(new TextBlock
            {
                Text = L("当前游戏配套文件状态", "Current Game Companion Library Status"),
                Foreground = new SolidColorBrush(Color.FromRgb(74, 222, 128)),
                FontWeight = FontWeights.SemiBold,
                FontSize = 12,
                Margin = new Thickness(0, 0, 0, 4)
            });
            var summaryText = (_compatSummary != null && !string.IsNullOrEmpty(_compatSummary.Text))
                ? _compatSummary.Text
                : L("选择游戏后自动检查配套运行库文件。", "Select a game to check its companion runtime files automatically.");
            statusStack.Children.Add(new TextBlock
            {
                Text = summaryText,
                TextWrapping = TextWrapping.Wrap,
                Foreground = new SolidColorBrush(Color.FromRgb(202, 208, 220)),
                FontSize = 12
            });
            statusBox.Child = statusStack;
            panel.Children.Add(statusBox);

            var qaText = L(
                "【兼容修复常见问题 Q&A】\n\n" +
                "1. 什么时候需要使用兼容修复？\n" +
                "   • 游戏设置中的 DLSS 帧生成 / XeSS / FSR 选项灰显、无法开启或提示硬件不支持（部分游戏自带了较旧的配套 DLL）。\n" +
                "   • 开启插帧后游戏闪退、报错，或插帧倍率受限。\n" +
                "   • 游戏更新后自带的文件与 OptiScaler 补丁发生版本冲突。\n\n" +
                "2. 兼容修复做了什么？\n" +
                "   • 自动备份：将游戏目录中原有的配套 DLL（libxess.dll、amd_fidelityfx_*.dll 等）完整备份到游戏目录下的 OptiScaler-compat-backup 文件夹中。\n" +
                "   • 配套替换：将本工具箱经过实测验证的配套运行库一键部署到游戏目录，替换厂商未更新的老旧文件，使补丁与游戏运行库完美协同。\n\n" +
                "3. 如何还原或应对游戏更新？\n" +
                "   • 若修复后没有改善或出现异常，随时点击“恢复原文件”即可一键无损还原。\n" +
                "   • 游戏更新后若正常运行，可在还原窗口中选择“保留当前文件并结束修复”，安全清理旧记录。",
                "[Compatibility Repair Q&A]\n\n" +
                "1. When should you use Compatibility Repair?\n" +
                "   • In-game DLSS Frame Generation / XeSS / FSR options are greyed out, cannot be toggled on, or show hardware unsupported errors (some games bundle outdated companion DLLs).\n" +
                "   • Game crashes or throws errors when frame generation is enabled, or frame rate multipliers are limited.\n" +
                "   • Game updates introduced conflicting companion libraries.\n\n" +
                "2. What does Compatibility Repair do?\n" +
                "   • Automatic Backup: Safely backs up original companion DLLs (libxess.dll, amd_fidelityfx_*.dll, etc.) to the OptiScaler-compat-backup folder inside the game directory.\n" +
                "   • Companion Replacement: Deploys validated runtime libraries bundled with this toolbox, replacing stale vendor files so the mod and game work seamlessly together.\n\n" +
                "3. How to restore or handle game updates?\n" +
                "   • If repair does not help, click 'Restore Files' anytime for one-click lossless rollback.\n" +
                "   • After a game update, you can choose 'Keep current files and finish repair' to safely keep the latest files."
            );
            panel.Children.Add(new TextBlock
            {
                Text = qaText,
                TextWrapping = TextWrapping.Wrap,
                Foreground = Brushes.WhiteSmoke,
                Margin = new Thickness(0, 0, 0, 20)
            });

            var row = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right };
            var ok = new Button { Content = L("确定", "OK"), MinWidth = 90, Margin = new Thickness(4) };
            DialogTheme.Primary(dialog, ok);
            ok.Click += delegate { dialog.DialogResult = true; };
            row.Children.Add(ok);
            panel.Children.Add(row);
            DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        void ShowCompatibilityHelp()
        {
            CreateCompatibilityHelpDialog().ShowDialog();
        }

        void UpdateCompatibilitySummary()
        {
            var busy = _writeBusy || (_session != null && _session.IsRunning);
            var report = _lastInspectResult == null ? null : _lastInspectResult.Compatibility;
            var available = _selectedGame != null && report != null && !busy;
            if (_compatButton != null) _compatButton.IsEnabled = available;
            // Keep restoration reachable even when inspection reports damaged package inputs.
            if (_restoreCompatButton != null) _restoreCompatButton.IsEnabled = available && (report.Active || !string.IsNullOrEmpty(report.Error));
            if (_compatSummary == null) return;
            if (_selectedGame == null || report == null) {
                _compatSummary.Text = L("选择游戏后自动检查配套运行库文件。", "Select a game to check its companion runtime files automatically.");
                return;
            }
            if (!string.IsNullOrEmpty(report.Error)) {
                _compatSummary.Text = L("配套文件检查未完成：", "Companion files inspection incomplete: ") + report.Error;
                return;
            }
            if (report.Active) {
                _compatSummary.Text = L("已有兼容修复备份。无改善可点“恢复游戏原文件”；游戏已更新时，也可在该窗口保留当前文件并结束旧修复，再重新修复。", "A compatibility repair backup exists. Use Restore game files if it did not help. After a game update, that dialog also lets you keep current files and finish the old repair before trying again.");
                return;
            }
            var names = new List<string>();
            if (report.Groups != null) foreach (var group in report.Groups)
                if (group.Files != null) foreach (var file in group.Files)
                    if (file.State == "different") names.Add(file.Name);
            _compatSummary.Text = names.Count > 0
                ? L("游戏目录与包内配套文件不同：", "Game and package companion files differ: ") + string.Join(", ", names) +
                    L("。版本不同不等于故障。若游戏中 DLSS-FG/XeSS/FSR 不可用、插帧不可开启或启动异常，可点“兼容修复”查看并尝试替换。", ". Different versions do not prove a fault. If in-game DLSS-FG/XeSS/FSR is unavailable, frame gen is disabled or startup fails, click Compatibility repair to review and try replacement.")
                : L("若游戏中 DLSS-FG/XeSS/FSR 不可用、插帧不可开启或启动异常，可点“兼容修复”检查；无需自行寻找 DLL。", "If in-game DLSS-FG/XeSS/FSR is unavailable, frame gen is disabled or startup fails, click Compatibility repair to review; no manual DLL search is needed.");
        }

        Window CreateCompatibilityDialog(bool restore)
        {
            var report = _lastInspectResult == null ? null : _lastInspectResult.Compatibility;
            var dialog = new Window {
                Owner = _window.IsVisible ? _window : null,
                Title = restore ? L("恢复游戏原文件", "Restore game files") : L("兼容修复与配套库替换", "Compatibility repair & companion replacement"),
                Width = 650, SizeToContent = SizeToContent.Height, MaxHeight = 720,
                WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize,
                ShowInTaskbar = false, Background = new SolidColorBrush(Color.FromRgb(32,35,43)), Foreground = Brushes.White
            };
            DialogTheme.Apply(dialog, _window);
            var panel = new StackPanel { Margin = new Thickness(24) };
            Action<string> text = value => panel.Children.Add(new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,12) });
            text(_selectedGame == null ? "" : _selectedGame.Exe);
            text(restore
                ? L("关闭游戏后恢复上次修复前的文件，并移除修复时新增的文件。游戏更新过的文件不会被覆盖。若游戏已更新或希望保留现状，可选“保留当前文件并结束修复”：不恢复任何 DLL，保留全部备份，并允许再次修复。", "Close the game first. Restore files from before the last repair and remove files added by it. Files changed by game updates will be preserved. Alternatively, Keep current files and finish repair changes no DLLs, retains all backups and allows a new repair.")
                : L("关闭游戏后，将自动把游戏目录中的配套运行库（XeSS、AMD FidelityFX 等）安全备份到 OptiScaler-compat-backup，并替换为本工具箱经过验证的最新配套组件。\n解决部分游戏中设置里无法开启 DLSS 帧生成 / FSR / XeSS，或因游戏自带 DLL 老旧冲突导致的异常。\n无改善可随时点击“恢复原文件”一键还原。", "Close the game first. Safely back up game companion runtime libraries (XeSS, AMD FidelityFX, etc.) into OptiScaler-compat-backup, and replace them with tested companion components bundled with this toolbox.\nSolves issues in some games where in-game DLSS-FG / FSR / XeSS cannot be toggled on or conflicts with outdated bundled DLLs.\nIf it does not help, you can restore original files at any time."));
            var selections = new Dictionary<string, CheckBox>();
            if (report != null && !string.IsNullOrEmpty(report.Error)) text(report.Error);
            var canRestore = restore && report != null && report.Active && !string.IsNullOrEmpty(report.Fingerprint);
            var proceed = new Button { Content = restore ? L("恢复原文件", "Restore originals") : L("备份并尝试修复", "Back up and try repair"), IsEnabled = canRestore, MinWidth = 150, Margin = new Thickness(4) };
            DialogTheme.Primary(dialog, proceed);
            if (restore && report != null && report.RestoreFiles != null) text(string.Join("\n", report.RestoreFiles));
            if (!restore && report != null && report.Groups != null) {
                if (report.Groups.Length > 1) {
                    var selectAllBtn = new Button {
                        Content = L("全选所有就绪组件", "Select all ready components"),
                        HorizontalAlignment = HorizontalAlignment.Left,
                        Margin = new Thickness(0, 0, 0, 8),
                        Padding = new Thickness(8, 4, 8, 4)
                    };
                    selectAllBtn.Click += delegate {
                        foreach (var item in selections.Values) {
                            if (item.IsEnabled) item.IsChecked = true;
                        }
                    };
                    panel.Children.Add(selectAllBtn);
                }
                foreach (var group in report.Groups) {
                    var check = new CheckBox { Content = XessGroupLabel(group.Id), Foreground = Brushes.White,
                        IsEnabled = group.Ready && !report.Active, Margin = new Thickness(0,6,0,6) };
                    selections[group.Id] = check;
                    RoutedEventHandler changed = (s,e) => {
                        var any = false;
                        foreach (var item in selections.Values) if (item.IsChecked == true) any = true;
                        proceed.IsEnabled = any;
                    };
                    check.Checked += changed; check.Unchecked += changed;
                    panel.Children.Add(check);
                    if (!group.Ready) text(L("安装包缺少有效配套文件，请重新解压完整包。", "Valid companion files are missing from this package. Extract a complete package again."));
                    if (group.Files != null) foreach (var file in group.Files) {
                        var gameVersion = file.State == "missing" ? L("无", "absent") : string.IsNullOrEmpty(file.GameVersion) ? L("版本未知", "unknown version") : file.GameVersion;
                        var packageVersion = string.IsNullOrEmpty(file.PackageVersion) ? L("版本未知", "unknown version") : file.PackageVersion;
                        text(file.Name + "\n" + L("游戏目录：", "Game folder: ") + gameVersion +
                            L("    包内：", "    Package: ") + packageVersion +
                            (file.State == "same" ? L("（文件相同）", " (identical)") : L("（文件不同或缺失）", " (different or absent)")));
                    }
                }
            }
            if (!restore && report != null && report.Active) text(L("请先打开“恢复游戏原文件”，恢复上次修复，或保留当前文件并结束旧修复，再尝试其他组件。", "Open Restore game files first: restore the previous repair, or keep current files and finish it before trying other components."));
            if (report != null && report.Evidence != null && report.Evidence.Length > 0) {
                text(L("最近日志中的配套记录（可能来自之前的运行，不能单独证明本次故障）：", "Companion entries in the latest log (may be from a previous run; not proof of the current fault):"));
                text(string.Join("\n", report.Evidence));
            }
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            buttons.Children.Add(new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 90, Margin = new Thickness(4) });
            if (canRestore) {
                var keep = new Button { Content = L("保留当前文件并结束修复", "Keep current files and finish repair"), Margin = new Thickness(4) };
                keep.Click += delegate { dialog.Uid = "keepXess"; dialog.DialogResult = true; };
                buttons.Children.Add(keep);
            }
            proceed.Click += delegate {
                var chosen = new List<string>();
                foreach (var item in selections) if (item.Value.IsChecked == true) chosen.Add(item.Key);
                dialog.Tag = chosen.ToArray(); dialog.DialogResult = true;
            };
            buttons.Children.Add(proceed); panel.Children.Add(buttons);
            DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        void OnCompatibility(bool restore)
        {
            if (_selectedGame == null || _lastInspectResult == null || _writeBusy || (_session != null && _session.IsRunning)) return;
            var fingerprint = _lastInspectResult.Compatibility == null ? null : _lastInspectResult.Compatibility.Fingerprint;
            var dialog = CreateCompatibilityDialog(restore);
            if (dialog.ShowDialog() != true) return;
            _decisions.XessGroups = dialog.Tag as string[];
            ExecuteAction(dialog.Uid == "keepXess" ? "keepXess" : restore ? "restoreXess" : "repairXess", false, fingerprint);
        }

        string SelectMochizukiModels()
        {
            var dialog = CreateMochizukiModelDialog();
            return dialog.ShowDialog() == true ? dialog.Tag as string : null;
        }

        Window CreateMochizukiModelDialog()
        {
            BackendInfo capability = null;
            if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                foreach (var backend in _lastInspectResult.Backends)
                    if (backend != null && backend.Id == "mochizuki") { capability = backend; break; }
            var canExtract = capability != null && capability.PreparationState == "ready";
            var original = capability != null && capability.OriginalState == "ready";
            var small = capability != null && capability.SmallState == "ready";
            var dialog = new Window
            {
                Owner = _window.IsVisible ? _window : null, Title = L("选择 Mochizuki 模型", "Select Mochizuki models"), Width = 500,
                SizeToContent = SizeToContent.Height, WindowStartupLocation = WindowStartupLocation.CenterOwner,
                ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false,
                Background = new SolidColorBrush(Color.FromRgb(32, 35, 43)), Foreground = Brushes.White
            };
            DialogTheme.Apply(dialog, _window);
            var panel = new StackPanel { Margin = new Thickness(24) };
            panel.Children.Add(new TextBlock
            {
                Text = L("请选择要安装的模型，不会自动勾选任何选项。", "Choose the models to install. No option is selected automatically."),
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 12)
            });
            panel.Children.Add(new TextBlock
            {
                Text = L("新模型需要 nvngx_dlssnr.dll 310.9.16.0 或已验证的新版权重；原始模型使用旧版权重。来源缺失或无效时会停止安装。", "The new model requires nvngx_dlssnr.dll 310.9.16.0 or verified new weights. Original uses older weights. Missing or invalid sources stop installation."),
                TextWrapping = TextWrapping.Wrap, Foreground = Brushes.Orange,
                Margin = new Thickness(0, 0, 0, 16)
            });
            var proceed = new Button { Content = L("继续", "Continue"), IsEnabled = false, MinWidth = 100, Margin = new Thickness(4) };
            DialogTheme.Primary(dialog, proceed);
            if (!canExtract && !(original && small))
                panel.Children.Add(new TextBlock {
                    Text = L("模型提取辅助组件缺失，请重新解压完整安装包。已有模型仍可选择。", "Model extraction helper is unavailable. Extract the complete package again. Existing models remain available."),
                    TextWrapping = TextWrapping.Wrap, Foreground = Brushes.OrangeRed, Margin = new Thickness(0,0,0,12)
                });
            var enabled = new[] { (original && small) || canExtract, small || canExtract, original || canExtract };
            var values = new[] { "both", "small", "original" };
            var labels = new[] { L("两个模型（推荐）", "Both models (recommended)"), L("仅新模型", "New model only"), L("仅原始模型", "Original model only") };
            for (var i = 0; i < values.Length; i++)
            {
                var value = values[i];
                var radio = new RadioButton { Content = labels[i], IsEnabled = enabled[i], Foreground = enabled[i] ? Brushes.White : Brushes.Gray, GroupName = "MochizukiModels", Margin = new Thickness(0, 6, 0, 6) };
                radio.Checked += delegate { dialog.Tag = value; proceed.IsEnabled = true; };
                panel.Children.Add(radio);
            }
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            var cancel = new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 100, Margin = new Thickness(4) };
            proceed.Click += delegate { dialog.DialogResult = true; };
            buttons.Children.Add(cancel);
            buttons.Children.Add(proceed);
            panel.Children.Add(buttons);
            DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        Window CreateLmxxfModelDialog()
        {
            BackendInfo capability = null;
            if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                foreach (var backend in _lastInspectResult.Backends)
                    if (backend != null && backend.Id == "lmxxf") { capability = backend; break; }
            var original = capability != null && (capability.OriginalState == "ready" || capability.OriginalState == "preparable");
            var small = capability != null && (capability.SmallState == "ready" || capability.SmallState == "preparable");
            var needsOriginalPreparation = capability != null && capability.OriginalState == "preparable";
            var needsSmallPreparation = capability != null && capability.SmallState == "preparable";
            var dialog = new Window {
                Owner = _window.IsVisible ? _window : null, Title = L("选择 lmxxf 模型", "Select lmxxf models"), Width = 540,
                SizeToContent = SizeToContent.Height, WindowStartupLocation = WindowStartupLocation.CenterOwner,
                ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false,
                Background = new SolidColorBrush(Color.FromRgb(32,35,43)), Foreground = Brushes.White
            };
            DialogTheme.Apply(dialog, _window);
            var panel = new StackPanel { Margin = new Thickness(24) };
            panel.Children.Add(new TextBlock { Text = L("请选择要安装的模型。现有的另一套模型会保留。", "Choose the models to install. Other installed models are retained."), TextWrapping = TextWrapping.Wrap });
            panel.Children.Add(new TextBlock { Text = "Original: " +
                (needsOriginalPreparation ? L("未安装（可提取）", "Not installed (can extract)") : original ? L("就绪", "Ready") : L("缺失", "Missing")) + "  |  Small: " +
                (needsSmallPreparation ? L("未安装（可提取）", "Not installed (can extract)") : small ? L("已安装", "Installed") : L("缺失", "Missing")), Margin = new Thickness(0,10,0,10) });
            if (capability != null && !string.IsNullOrEmpty(BackendHintText(capability)))
                panel.Children.Add(new TextBlock { Text = BackendHintText(capability), TextWrapping = TextWrapping.Wrap, Foreground = Brushes.Orange, Margin = new Thickness(0,0,0,10) });
            var proceed = new Button { Content = L("继续", "Continue"), IsEnabled = false, MinWidth = 100, Margin = new Thickness(4) };
            DialogTheme.Primary(dialog, proceed);
            var values = new[] { "both", "small", "original" };
            var labels = new[] {
                (needsOriginalPreparation && needsSmallPreparation)
                    ? L("两个模型（从来源提取 Original 与 Small）", "Both models (extract Original and Small from source)")
                    : needsSmallPreparation
                    ? L("两个模型（复用 Original，提取 Small）", "Both models (reuse Original, extract Small)")
                    : needsOriginalPreparation
                    ? L("两个模型（提取 Original，复用 Small）", "Both models (extract Original, reuse Small)")
                    : L("两个模型", "Both models"),
                needsSmallPreparation ? L("Small（从来源提取并安装）", "Small (extract and install from source)") : "Small (310.9.16)",
                needsOriginalPreparation ? L("Original（从来源提取并安装）", "Original (extract and install from source)") : L("Original（仅复用原始模型）", "Original (reuse existing model only)") };
            var enabled = new[] { original && small, small, original };
            for (var i = 0; i < values.Length; i++) {
                var value = values[i];
                var radio = new RadioButton { Content = labels[i], Foreground = enabled[i] ? Brushes.White : Brushes.Gray, IsEnabled = enabled[i], GroupName = "LmxxfModels", Margin = new Thickness(0,6,0,6) };
                radio.Checked += delegate { dialog.Tag = value; proceed.IsEnabled = true; };
                panel.Children.Add(radio);
            }
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            var cancel = new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 100, Margin = new Thickness(4) };
            proceed.Click += delegate { if (dialog.Tag != null) dialog.DialogResult = true; };
            buttons.Children.Add(cancel); buttons.Children.Add(proceed); panel.Children.Add(buttons); DialogTheme.SetContent(dialog, panel);
            return dialog;
        }

        string SelectLmxxfModels()
        {
            var dialog = CreateLmxxfModelDialog();
            return dialog.ShowDialog() == true ? dialog.Tag as string : null;
        }

        bool PrepareMissingModelSource(string action, List<string> selected)
        {
            var needsSource = false;
            if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                foreach (var backend in _lastInspectResult.Backends) {
                    if (backend == null || !selected.Contains(backend.Id)) continue;
                    if (backend.Id == "mochizuki" && backend.RuntimeState == "ready" && backend.ModelState == "missing") needsSource = true;
                    if (backend.Id == "lmxxf" && backend.RuntimeState == "ready" &&
                        backend.OriginalState != "ready" && backend.OriginalState != "preparable" &&
                        backend.SmallState != "ready" && backend.SmallState != "preparable") needsSource = true;
                    if (backend.Id == "daniel" && backend.ModelState == "missing" &&
                        (backend.RuntimeState == "ready" || (backend.Hints != null && Array.IndexOf(backend.Hints, "daniel-setup") < 0))) needsSource = true;
                }
            if (!needsSource) return true;
            if (!string.IsNullOrEmpty(_decisions.ModelSourceDll))
                DialogTheme.Show(_window, L("所选 DLL 无法提供需要的模型。请选择受支持的 nvngx_dlssnr.dll；Small 需要 310.9.16。", "The selected DLL cannot supply the required model. Choose a supported nvngx_dlssnr.dll; Small requires 310.9.16."), _window.Title);
            var directory = Path.GetDirectoryName(_selectedGame.Exe);
            var picker = new OpenFileDialog {
                Title = L("缺少模型来源：选择 nvngx_dlssnr.dll（用于离线提取）", "Model source missing: select nvngx_dlssnr.dll for offline extraction"),
                Filter = "nvngx_dlssnr.dll|nvngx_dlssnr.dll|DLL (*.dll)|*.dll", CheckFileExists = true,
                InitialDirectory = directory, FileName = Path.Combine(directory, "nvngx_dlssnr.dll"), RestoreDirectory = true
            };
            if (picker.ShowDialog(_window) != true) return false;
            _decisions.ModelSourceDll = picker.FileName;
            _resumeInstallAction = action;
            ReleaseLock();
            // Inspect the selected source before showing model choices or writing game files.
            InspectGame(_selectedGame);
            return false;
        }

        void ExecuteAction(string action, bool keepModelChoice = false, string compatibilityFingerprint = null)
        {
            var installing = action == "install" || action == "update";
            if (_selectedGame == null || !_selectedGame.HasExecutable) return;
            if (_writeBusy || _folderScan != null || (_session != null && _session.IsRunning))
            {
                DialogTheme.Show(_window, _language == "zh" ? "操作正在进行中…" : "An operation is already in progress.", _window.Title);
                return;
            }

            // Decisions are valid only for this attempt and its explicit needsInput retries.
            if (!keepModelChoice) _decisions.ForeignProxy.Clear();
            var dir = Path.GetDirectoryName(_selectedGame.Exe);
            if (!TryLockTarget(dir))
            {
                if (_statusText != null)
                    _statusText.Text = _language == "zh" ? "目标文件夹已被另一个安装进程锁定。" : "Target folder is locked by another launcher.";
                return;
            }

            var proxy = CurrentSelectedProxy;
            if (installing && string.IsNullOrEmpty(proxy))
            {
                DialogTheme.Show(_window, _language == "zh" ? "请先选择要注入的代理 DLL。" : "Please select a proxy DLL to inject first.", _window.Title);
                ReleaseLock();
                return;
            }

            var checkedBackends = new List<string>();
            if (_lmxxfCheck != null && _lmxxfCheck.IsChecked == true) checkedBackends.Add("lmxxf");
            if (_danielCheck != null && _danielCheck.IsChecked == true) checkedBackends.Add("daniel");
            if (_mochiCheck != null && _mochiCheck.IsChecked == true) checkedBackends.Add("mochizuki");

            if (installing && checkedBackends.Count == 0)
            {
                DialogTheme.Show(_window, _language == "zh" ? "请至少勾选一个可安装的后端。" : "Please select at least one backend to install.", _window.Title);
                ReleaseLock();
                return;
            }
            if (installing && !PrepareMissingModelSource(action, checkedBackends)) { ReleaseLock(); return; }

            if (installing && checkedBackends.Contains("lmxxf") &&
                (!keepModelChoice || string.IsNullOrEmpty(_decisions.LmxxfModels)))
            {
                _decisions.LmxxfModels = SelectLmxxfModels();
                if (string.IsNullOrEmpty(_decisions.LmxxfModels)) { ReleaseLock(); return; }
            }
            if (installing && checkedBackends.Contains("mochizuki") &&
                (!keepModelChoice || string.IsNullOrEmpty(_decisions.MochizukiModels)))
            {
                _decisions.MochizukiModels = SelectMochizukiModels();
                if (string.IsNullOrEmpty(_decisions.MochizukiModels)) { ReleaseLock(); return; }
            }

            SetBusy(true, action);
            if (_logView != null) _logView.Clear();
            if (_statusLed != null) _statusLed.Fill = WarningBrush;
            if (_progress != null) _progress.IsIndeterminate = true;
            _pendingAction = action;

            _decisions.ExistingIni = (_replaceIni != null && _replaceIni.IsChecked == true) ? "overwrite" : "keep";

            var req = new LauncherRequest
            {
                RequestId = Guid.NewGuid().ToString("N"),
                Action = action,
                Language = _language,
                GameExe = _selectedGame.Exe,
                ProxyChoice = proxy,
                Backends = installing ? checkedBackends.ToArray() : null,
                ActiveBackend = installing ? _activeBackend : null,
                PackageRoot = _packageRoot,
                ExplicitDecisions = _decisions,
                Fingerprint = (action != "inspect" && _lastInspectResult != null) ? _lastInspectResult.Fingerprint : null,
                CompatibilityFingerprint = compatibilityFingerprint ?? (_lastInspectResult != null && _lastInspectResult.Compatibility != null ? _lastInspectResult.Compatibility.Fingerprint : null)
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
                if (ex.Data.Contains("logPath")) _currentLogPath = Convert.ToString(ex.Data["logPath"]);
                if (_statusText != null) _statusText.Text = ex.Message;
                AppendLog(ex.Message);
            }
        }

        void OnEvent(string stage, string message)
        {
            _window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (stage != "log") _eventStage = stage;
                if (_statusText != null) _statusText.Text = StageText(_eventStage);
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

                var action = (source != null && !string.IsNullOrEmpty(source.Action)) ? source.Action : _pendingAction;
                _pendingAction = "";
                Detach(source);
                _session = null;
                ReleaseLock();

                if (action == "inspect" && (_selectedGame == null || !_selectedGame.HasExecutable ||
                    !SamePath(source.GameExe, _selectedGame.Exe))) {
                    _resumeInstallAction = "";
                    UpdatePrimaryButtonState();
                    return;
                }

                if (result != null && !string.IsNullOrEmpty(result.LogPath))
                    _currentLogPath = result.LogPath;

                if (result == null)
                {
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    if (_statusText != null) _statusText.Text = L("执行失败（退出代码 " + code + "）。", "Worker failed (exit " + code + ").");
                }
                else if (string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase) && code != 0)
                {
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    if (_statusText != null) _statusText.Text = L("执行退出代码 " + code + " 与状态 " + result.Status + " 不一致。", "Worker exit " + code + " does not match status " + result.Status);
                }
                else
                {
                    if (_statusLed != null) _statusLed.Fill = ReadyBrush;
                    if (_statusText != null) _statusText.Text = result.Status + (string.IsNullOrEmpty(result.Error) ? "" : (": " + result.Error));
                    RefreshStatusLanguage();
                }

                if (result != null && result.Compatibility != null && _lastInspectResult != null &&
                    (action == "repairXess" || action == "restoreXess" || action == "keepXess"))
                    _lastInspectResult.Compatibility = result.Compatibility;

                if (string.Equals(action, "inspect", StringComparison.OrdinalIgnoreCase))
                {
                    ApplyInspect(result);
                    var resume = _resumeInstallAction;
                    _resumeInstallAction = "";
                    if (!string.IsNullOrEmpty(resume) && code == 0 && result != null && result.Status == "success") {
                        ExecuteAction(resume);
                        return;
                    }
                }
                else if (result != null && string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase))
                {
                    if (action == "repairXess" || action == "restoreXess" || action == "keepXess") {
                        if (_lastInspectResult != null) _lastInspectResult.Compatibility = result.Compatibility;
                        DialogTheme.Show(_window, action == "repairXess"
                            ? L("已备份并替换所选配套运行库文件（XeSS / AMD FidelityFX 等）。请启动游戏测试；无改善可回到这里点“恢复游戏原文件”。", "Selected companion files (XeSS / AMD FidelityFX, etc.) were backed up and replaced. Start the game to test. If it did not help, return here and choose Restore game files.")
                            : action == "keepXess"
                            ? L("已保留当前游戏文件并结束旧修复，全部备份仍保留。现在可以重新进行兼容修复。", "Current game files and all backups were kept. The old repair is finished; you can now perform compatibility repair again.")
                            : L("游戏原文件已恢复。请重新启动游戏。", "Original game files restored. Restart the game."), _window.Title);
                        if (_selectedGame != null) InspectGame(_selectedGame);
                    }
                    if (action == "install" || action == "update" || action == "uninstall")
                    {
                        if (action == "uninstall")
                        {
                            if (_selectedGame != null) _selectedGame.Installed = false;
                        }
                        else
                        {
                            if (_selectedGame != null) _selectedGame.Installed = true;
                        }
                        SaveLibraryCache();
                        RefreshGameListView();
                        if (_selectedGame != null) InspectGame(_selectedGame);
                    }
                }

                AppendLog(_statusText != null ? _statusText.Text : "");

                if (result != null && string.Equals(result.Status, "needsInput", StringComparison.OrdinalIgnoreCase))
                {
                    AskDecisions(result, action);
                }

                UpdatePrimaryButtonState();
                if (action != "inspect" && result != null && result.Status == "failed") {
                    var detail = result.Error ?? "";
                    if (detail.Length > 1200) detail = detail.Substring(0, 1200) + "…";
                    if (_statusLed != null) _statusLed.Fill = WarningBrush;
                    DialogTheme.Show(_window, L("操作失败：", "Operation failed: ") + detail +
                        "\n\n" + L("完整日志：", "Full log: ") + _currentLogPath, _window.Title, MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }));
        }

        void ApplyInspect(LauncherResult result)
        {
            if (result == null) return;
            _lastInspectResult = result;
            if (!string.IsNullOrEmpty(result.Status) && result.Status != "success") {
                RefreshInspectionText();
                UpdatePrimaryButtonState();
                return;
            }
            var isZh = _language == "zh";
            UpdateProxyRecommendation();

            if (_injectText != null)
            {
                _injectText.Text = isZh
                    ? "分析完成，请确认注入代理与后端配置："
                    : "Analysis complete, please confirm proxy DLL and backends:";
            }

            // Checkbox events can temporarily fall back while capabilities are applied
            // in worker order. Resolve the preferred backend after the complete batch.
            var preferredActiveBackend = _activeBackend;
            var hasUserChoice = CurrentBackendChoice() != null;
            _updatingBackends = true;
            try
            {
                if (result.Backends != null)
                {
                    foreach (var b in result.Backends)
                    {
                        if (b == null) continue;
                        var reasonText = GetReasonText(b.ReasonCode, b.Reason, isZh);
                        if (b.Id == "lmxxf")
                        {
                            if (_lmxxfCheck != null)
                            {
                                _lmxxfCheck.IsEnabled = true;
                                if (!hasUserChoice) _lmxxfCheck.IsChecked = b.Enabled;
                            }
                            if (_lmxxfReason != null)
                            {
                                _lmxxfReason.Text = reasonText;
                                _lmxxfReason.Foreground = b.Enabled ? ReadyBrush : MissingBrush;
                            }
                            if (_lmxxfBadge != null)
                            {
                                _lmxxfBadge.Background = b.Enabled ? ReadyBg : MissingBg;
                                _lmxxfBadge.BorderBrush = b.Enabled ? ReadyBorder : MissingBorder;
                            }
                        }
                        else if (b.Id == "daniel")
                        {
                            if (_danielCheck != null)
                            {
                                _danielCheck.IsEnabled = true;
                                if (!hasUserChoice) _danielCheck.IsChecked = b.Enabled;
                            }
                            if (_danielReason != null)
                            {
                                _danielReason.Text = reasonText;
                                _danielReason.Foreground = b.Enabled ? ReadyBrush : MissingBrush;
                            }
                            if (_danielBadge != null)
                            {
                                _danielBadge.Background = b.Enabled ? ReadyBg : MissingBg;
                                _danielBadge.BorderBrush = b.Enabled ? ReadyBorder : MissingBorder;
                            }
                        }
                        else if (b.Id == "mochizuki")
                        {
                            if (_mochiCheck != null)
                            {
                                _mochiCheck.IsEnabled = true;
                                if (!hasUserChoice) _mochiCheck.IsChecked = b.Enabled;
                            }
                            if (_mochiReason != null)
                            {
                                _mochiReason.Text = reasonText;
                                _mochiReason.Foreground = b.Enabled ? ReadyBrush : MissingBrush;
                            }
                            if (_mochiBadge != null)
                            {
                                _mochiBadge.Background = b.Enabled ? ReadyBg : MissingBg;
                                _mochiBadge.BorderBrush = b.Enabled ? ReadyBorder : MissingBorder;
                            }
                        }
                    }
                }

                _activeBackend = preferredActiveBackend;
                UpdateActiveCombo();
            }
            finally { _updatingBackends = false; }
            RefreshBackendHints();
            UpdatePrimaryButtonState();

            if (result.Notes != null)
            {
                foreach (var n in result.Notes) AppendLog(n);
            }
        }

        bool AskDecisions(LauncherResult result, string action)
        {
            if (result.RequiredDecisions == null) return false;
            foreach (var d in result.RequiredDecisions)
            {
                if (d.Id == "foreignProxy" && d.Options != null && d.Options.Length > 0)
                {
                    var prompt = L(d.File + " 已存在，且不属于本项目。\n\n是 = 备份后替换，否 = 保留，取消 = 停止操作。",
                        d.File + " exists and is not this project.\n\nYes = backup, No = keep, Cancel = abort.");
                    var pick = DialogTheme.Show(_window, prompt, _window.Title, MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
                    if (_decisions.ForeignProxy == null) _decisions.ForeignProxy = new Dictionary<string, string>();
                    if (pick == MessageBoxResult.Yes) _decisions.ForeignProxy[d.File] = "backup";
                    else if (pick == MessageBoxResult.No) _decisions.ForeignProxy[d.File] = "keep";
                    else
                    {
                        if (_statusText != null) _statusText.Text = L("已取消。", "Cancelled.");
                        return false;
                    }
                }
                else if (d.Id == "lmxxfModels")
                {
                    _decisions.LmxxfModels = SelectLmxxfModels();
                    if (string.IsNullOrEmpty(_decisions.LmxxfModels)) return false;
                }
                else if (d.Id == "mochizukiModels")
                {
                    _decisions.MochizukiModels = SelectMochizukiModels();
                    if (string.IsNullOrEmpty(_decisions.MochizukiModels)) return false;
                }
                else if (d.Id == "reinspect")
                {
                    _decisions.ForeignProxy.Clear();
                    if (_selectedGame != null) InspectGame(_selectedGame);
                    return true;
                }
            }
            ExecuteAction(string.IsNullOrEmpty(action) ? "install" : action, true);
            return true;
        }

        void OpenLog()
        {
            try
            {
                if (!string.IsNullOrEmpty(_currentLogPath) && File.Exists(_currentLogPath))
                {
                    try
                    {
                        var psi = new ProcessStartInfo
                        {
                            FileName = _currentLogPath,
                            UseShellExecute = true
                        };
                        Process.Start(psi);
                        return;
                    }
                    catch { }

                    try
                    {
                        var winDir = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
                        var classicNotepad = Path.Combine(winDir, "notepad.exe");
                        if (File.Exists(classicNotepad))
                        {
                            Process.Start(classicNotepad, "\"" + _currentLogPath + "\"");
                            return;
                        }
                    }
                    catch { }

                    Process.Start("explorer.exe", "/select,\"" + _currentLogPath + "\"");
                }
                else
                {
                    DialogTheme.Show(_window, _language == "zh" ? "当前尚无可用日志。" : "No log file available yet.", _window.Title);
                }
            }
            catch (Exception ex)
            {
                DialogTheme.Show(_window, ex.Message, _window.Title);
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
            _writeBusy = busy && action != "inspect" && action != "scan";
            if (_gameList != null) _gameList.IsEnabled = !_writeBusy;
            if (_packageBrowse != null) _packageBrowse.IsEnabled = !_writeBusy;
            if (_packageRootBox != null) _packageRootBox.IsEnabled = !_writeBusy;
            if (busy) _eventStage = action;
            if (_addButton != null) _addButton.IsEnabled = !busy;
            if (_removeGameMenuItem != null) _removeGameMenuItem.IsEnabled = !busy && _removeGameMenuItem.Tag is GameInfo;
            if (_openFolderButton != null) _openFolderButton.IsEnabled = !busy;
            if (_refreshButton != null) _refreshButton.IsEnabled = !busy;
            if (_primaryButton != null) _primaryButton.IsEnabled = !busy;
            if (_uninstallButton != null) _uninstallButton.IsEnabled = !busy && (_selectedGame != null && _selectedGame.Installed);
            if (_proxyCombo != null) _proxyCombo.IsEnabled = !busy;
            if (_proxyPills != null)
            {
                foreach (var p in _proxyPills.Values) p.IsEnabled = !busy;
            }
            RefreshLibraryActions();
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
            // Keep the target mutex until the write completion callback has run,
            // even if the process has exited but its result is still queued.
            if (_writeBusy) {
                e.Cancel = true;
                if (_statusText != null) _statusText.Text = L("安装或卸载正在进行，请等待完成后关闭。", "Installation or removal is in progress. Wait for completion before closing.");
                return;
            }
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
                    e.Cancel = true;
                }
            }
            if (!e.Cancel) {
                if (_folderScan != null) _folderScan.Cancel();
                if (_recommendations != null) _recommendations.Dispose();
                ReleaseLock();
            }
        }
    }
}
