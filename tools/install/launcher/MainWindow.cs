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
        public string StatusText
        {
            get
            {
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
        public string Cover { get { return Info != null ? Info.Cover : null; } }
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
        public string Hero { get { return Info != null ? Info.Hero : null; } }
    }

    public sealed class MainController
    {
        readonly Window _window;
        TextBox _searchBox, _logView, _packageRootBox;
        Button _minBtn, _maxBtn, _closeBtn;
        Button _libraryTab, _settingsTab, _addButton, _primaryButton, _changeExeButton, _openFolderButton, _uninstallButton, _openLogButton, _refreshButton, _langToggle, _packageBrowse;
        FrameworkElement _libraryPage, _settingsPage;
        ComboBox _filterCombo, _proxyCombo, _activeCombo, _langCombo;
        ListBox _gameList;
        TextBlock _pageTitle, _countBadge, _heroKicker, _heroTitle, _heroMeta, _heroPath, _heroPosterInitial, _injectText, _injectIcon, _injectRecommendation, _refundWarning;
        TextBlock _lmxxfReason, _danielReason, _mochiReason, _statusText, _countText;
        Border _lmxxfBadge, _danielBadge, _mochiBadge;
        Button _lmxxfActiveBtn, _danielActiveBtn, _mochiActiveBtn;
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
            _changeExeButton = (Button)window.FindName("ChangeExeButton");
            _openFolderButton = (Button)window.FindName("OpenFolderButton");
            _uninstallButton = (Button)window.FindName("UninstallButton");
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

            _langCombo = (ComboBox)window.FindName("LangCombo");
            _scanOnStartup = (CheckBox)window.FindName("ScanOnStartup");
            _refreshButton = (Button)window.FindName("RefreshButton");
            _packageRootBox = (TextBox)window.FindName("PackageRootBox");
            _packageBrowse = (Button)window.FindName("PackageBrowse");

            _settingsDir = settingsDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OptScalerNR", "Launcher");
            _settingsPath = Path.Combine(_settingsDir, "settings.json");
            _libraryPath = Path.Combine(_settingsDir, "library.json");

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
                foreach (var name in new[] { "dxgi.dll", "winmm.dll", "d3d12.dll", "winhttp.dll", "wininet.dll", "dbghelp.dll" })
                    _proxyCombo.Items.Add(name);
                _proxyCombo.SelectedIndex = 0;
                _proxyCombo.SelectionChanged += (s, e) =>
                {
                    var selected = Convert.ToString(_proxyCombo.SelectedItem);
                    SelectProxyPillOnly(selected);
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
            if (_changeExeButton != null) _changeExeButton.Click += (s, e) => ChangeExe();
            if (_openFolderButton != null) _openFolderButton.Click += (s, e) => OpenSelectedFolder();
            if (_primaryButton != null) _primaryButton.Click += (s, e) => OnPrimaryAction();
            if (_uninstallButton != null) _uninstallButton.Click += (s, e) => OnUninstall();
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

        void SwitchTab(bool toSettings)
        {
            if (_libraryPage != null) _libraryPage.Visibility = toSettings ? Visibility.Collapsed : Visibility.Visible;
            if (_settingsPage != null) _settingsPage.Visibility = toSettings ? Visibility.Visible : Visibility.Collapsed;
            if (_settingsTab != null)
            {
                _settingsTab.Content = toSettings ? (_language == "zh" ? "返回游戏库" : "Back to Library") : (_language == "zh" ? "设置" : "Settings");
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
                        _productVersion = " " + File.ReadAllText(c).Trim();
                        return;
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
                if (_window != null)
                    _window.Title = "OptScaler NR" + _productVersion + (isZh ? " - 本软件免费，如付费购买请立即退款！" : " - Free software; if purchased, request a refund immediately!");
                if (_refundWarning != null)
                    _refundWarning.Text = isZh ? "本软件免费，如付费购买请立即退款！" : "Free software; if purchased, request a refund immediately!";
                if (_pageTitle != null) _pageTitle.Text = isZh ? "游戏库" : "Library";
                if (_langToggle != null) _langToggle.Content = isZh ? "🌐 English" : "🌐 Chinese";
                if (_libraryTab != null) _libraryTab.Content = isZh ? "游戏库" : "Library";
                if (_settingsTab != null)
                    _settingsTab.Content = _settingsPage != null && _settingsPage.Visibility == Visibility.Visible
                        ? L("返回游戏库", "Back to Library") : L("设置", "Settings");
                if (_addButton != null) _addButton.Content = isZh ? "+ 添加游戏" : "+ Add Game";
                if (_changeExeButton != null) _changeExeButton.Content = isZh ? "更换 EXE" : "Change EXE";
                if (_openFolderButton != null) _openFolderButton.Content = isZh ? "打开目录" : "Open Folder";
                if (_uninstallButton != null) _uninstallButton.Content = isZh ? "卸载" : "Uninstall";
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
                SetLabel("DanielCheck", "Daniel · 0.3.0-0.6.0", "Daniel · 0.3.0-0.6.0");
                SetLabel("MochiCheck", "mochi · 0.0.6", "mochi · 0.0.6");
                SetLabel("LmxxfDescription", "适合 RX 9000 系列", "For RX 9000 series");
                SetLabel("DanielDescription", "适合 RX 6000/7000/9000 系列", "For RX 6000/7000/9000 series");
                SetLabel("MochiDescription", "适合 RX 9000 系列", "For RX 9000 series");
                SetLabel("DiagnosticsHeading", "配置与诊断", "Configuration & Diagnostics");
                SetLabel("LogHeading", "运行诊断日志", "Diagnostic Log");
                SetLabel("SettingsHeading", "设置", "Settings");
                SetLabel("LanguageHeading", "语言", "Language");
                SetLabel("PackageHeading", "安装包目录", "Package Folder");
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
            if (_injectText != null) _injectText.Text = result != null
                ? L("分析完成，请确认注入代理与后端配置：", "Analysis complete. Confirm the proxy DLL and backends:")
                : (_selectedGame != null
                    ? L("正在检查导入与后端…", "Checking imports and backends…")
                    : L("请从下方选择游戏启动时所加载的拦截代理 DLL：", "Choose the proxy DLL to load when the game starts:"));
            if (_injectRecommendation != null) _injectRecommendation.Text = result == null
                ? L("选择游戏后将分析 PE 导入表并自动匹配推荐代理。", "Select a game to inspect its imports and recommend a proxy.")
                : (!string.IsNullOrEmpty(result.Recommend)
                    ? L("检测到程序导入 " + result.Recommend + "，推荐使用该代理。", "Detected import for " + result.Recommend + "; this proxy is recommended.")
                    : L("无法从导入表判断首选 DLL，建议优先尝试 dxgi.dll。", "Cannot determine the preferred DLL. Try dxgi.dll first."));
            foreach (var id in new[] { "lmxxf", "daniel", "mochizuki" })
            {
                BackendInfo capability = null;
                if (result != null && result.Backends != null)
                    foreach (var candidate in result.Backends)
                        if (candidate != null && candidate.Id == id) { capability = candidate; break; }
                var reason = capability != null ? GetReasonText(capability.ReasonCode, capability.Reason, _language == "zh")
                    : L("未检查", "Not checked");
                var label = id == "lmxxf" ? _lmxxfReason : id == "daniel" ? _danielReason : _mochiReason;
                if (label != null) label.Text = reason;
            }
            RefreshBackendHints();
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
            foreach (var hint in backend.Hints ?? new string[0])
            {
                switch (hint)
                {
                    case "lmxxf-weights": lines.Add(L("缺少 lmxxf 权重。请将完整的 native-game-tiled-assets 文件夹放到安装包目录。", "lmxxf weights are missing. Place the complete native-game-tiled-assets folder beside Setup.bat.")); break;
                    case "lmxxf-original": lines.Add(L("缺少 lmxxf Original 权重。请将完整的原始模型权重放入 native-game-tiled-assets。", "lmxxf Original weights are missing. Place the complete Original weights in native-game-tiled-assets.")); break;
                    case "lmxxf-small": lines.Add(L("缺少 lmxxf Small 权重。请补齐 native-game-tiled-assets\\small，或提供 nvngx_dlssnr.dll 310.9.16。", "lmxxf Small weights are missing. Supply native-game-tiled-assets\\small or nvngx_dlssnr.dll 310.9.16.")); break;
                    case "lmxxf-small-preparable": lines.Add(L("已找到 Small 模型来源，安装时准备。", "Small model source found. Preparation will run during installation.")); break;
                    case "mochi-source": lines.Add(L("缺少 Mochi 模型。请将 nvngx_dlssnr.dll 放到安装包目录。", "Mochi models are missing. Place nvngx_dlssnr.dll beside Setup.bat.")); break;
                    case "mochi-preparable": lines.Add(L("已找到模型来源，安装时准备。", "Model source found. Models will be prepared during installation.")); break;
                    case "mochi-versions": lines.Add(L("310.9.16 支持新旧模型；310.8.0 仅支持原始模型。", "310.9.16: both models. 310.8.0: original only.")); break;
                    case "mochi-unsupported-source": lines.Add(L("此 nvngx_dlssnr.dll 不受支持，请更换版本。", "Unsupported nvngx_dlssnr.dll. Supply a supported version.")); break;
                    case "daniel-runtime": lines.Add(L("缺少 Daniel 运行库。请将 Daniel 的 version.dll 放到安装包目录。", "Daniel runtime is missing. Place Daniel's version.dll beside Setup.bat.")); break;
                    case "daniel-weights": lines.Add(L("缺少 Daniel 权重。请放入 dlssnr_on_amd_weights.bin，或补齐模型提取文件。", "Daniel weights are missing. Supply dlssnr_on_amd_weights.bin or the extraction files.")); break;
                    case "daniel-setup": lines.Add(L("请将 Daniel 的 dlssnr_on_amd_setup.exe 放到安装包目录。", "Place Daniel's dlssnr_on_amd_setup.exe beside Setup.bat.")); break;
                    case "daniel-source": lines.Add(L("请将受支持的 nvngx_dlssnr.dll 放到安装包目录。", "Place a supported nvngx_dlssnr.dll beside Setup.bat.")); break;
                    case "daniel-preparable": lines.Add(L("已找到提取文件，安装时准备。", "Extraction files found. Preparation will run during installation.")); break;
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
                    BackendChoices = _backendChoices
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
                        if (g != null && !string.IsNullOrEmpty(g.Exe) &&
                            (g.ExeOverride == true || g.Source == "Manual" || GamePaths.CanUse(g.Exe)))
                            _allGames.Add(g);
                    }
                    var count = _allGames.Count;
                    MergeScannedGames(new GameInfo[0]);
                    if (_allGames.Count != count) SaveLibraryCache();
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
                    Source = "Manual",
                    Installed = File.Exists(Path.Combine(dir, "amd-presr-install.txt"))
                };
                _allGames.Add(found);
            }

            // Picking an existing scanned entry is also an explicit EXE choice.
            found.ExeOverride = true;
            SaveLibraryCache();
            RefreshGameListView();
            SelectGame(found);
        }

        void ChangeExe()
        {
            if (_selectedGame == null) return;
            var picked = GamePaths.PickExe(_language == "zh");
            if (string.IsNullOrEmpty(picked)) return;
            var norm = GamePaths.Normalize(picked);
            if (string.IsNullOrEmpty(norm) || !GamePaths.CanUse(norm)) return;

            _selectedGame.Exe = norm;
            _selectedGame.ExeOverride = true;
            var dir = Path.GetDirectoryName(norm);
            _selectedGame.Installed = File.Exists(Path.Combine(dir, "amd-presr-install.txt"));
            SaveLibraryCache();

            RefreshGameListView();
            SelectGame(_selectedGame);
        }

        void OpenSelectedFolder()
        {
            if (_selectedGame == null || string.IsNullOrEmpty(_selectedGame.Exe)) return;
            try
            {
                var dir = Path.GetDirectoryName(_selectedGame.Exe);
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
                var exes = Directory.GetFiles(path, "*.exe", SearchOption.TopDirectoryOnly);
                if (exes.Length > 0) path = exes[0];
            }
            if (path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                AddGameFromPath(path);
            }
        }

        static ImageSource GetOrExtractIcon(GameInfo g)
        {
            if (g == null || string.IsNullOrEmpty(g.Exe)) return null;
            if (!string.IsNullOrEmpty(g.Cover) && File.Exists(g.Cover)) return null;
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
                    var matchName = g.Name != null && g.Name.ToLowerInvariant().Contains(query);
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
            _decisions.ForeignProxy.Clear();
            _selectedGame = game;
            _lastInspectResult = null;
            RestoreBackendChoice();
            RefreshInspectionText();
            if (_heroTitle != null) _heroTitle.Text = game.Name;
            if (_heroMeta != null) _heroMeta.Text = game.Source + " · PC";
            if (_heroPath != null) _heroPath.Text = game.Exe;

            if (_heroBgImage != null)
            {
                if (!string.IsNullOrEmpty(game.Hero) && File.Exists(game.Hero))
                {
                    try
                    {
                        var bi = new BitmapImage();
                        bi.BeginInit();
                        bi.CacheOption = BitmapCacheOption.OnLoad;
                        bi.UriSource = new Uri(game.Hero, UriKind.Absolute);
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
                if (!string.IsNullOrEmpty(game.Cover) && File.Exists(game.Cover))
                {
                    try
                    {
                        var bi = new BitmapImage();
                        bi.BeginInit();
                        bi.CacheOption = BitmapCacheOption.OnLoad;
                        bi.DecodePixelWidth = 180;
                        bi.UriSource = new Uri(game.Cover, UriKind.Absolute);
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
                    _heroPosterInitial.Text = string.IsNullOrEmpty(game.Name) ? "?" : game.Name.Substring(0, 1).ToUpperInvariant();
                    _heroPosterInitial.Visibility = Visibility.Visible;
                }
            }

            UpdatePrimaryButtonState();
            InspectGame(game);
        }

        void UpdatePrimaryButtonState()
        {
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

            _primaryButton.Content = _selectedGame.Installed
                ? (isZh ? "⚡ 更新 OptScaler NR" : "⚡ Update OptScaler NR")
                : (isZh ? "⚡ 安装 OptScaler NR" : "⚡ Install OptScaler NR");
            _primaryButton.IsEnabled = !isBusy && hasBackend && proxyChosen;
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
                AppendLog("Scan error: " + ex.Message);
            }
        }

        static bool SameLibraryGame(GameInfo a, GameInfo b)
        {
            return (!string.IsNullOrEmpty(a.Id) && !string.IsNullOrEmpty(b.Id) &&
                    string.Equals(a.Id, b.Id, StringComparison.OrdinalIgnoreCase)) ||
                   string.Equals(a.Exe, b.Exe, StringComparison.OrdinalIgnoreCase);
        }

        static bool IsAuxiliaryExe(string path)
        {
            var name = Path.GetFileName(path);
            return string.Equals(name, "gamelaunchhelper.exe", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(name, "person-worker.exe", StringComparison.OrdinalIgnoreCase) ||
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
                if (cached == null || string.IsNullOrEmpty(cached.Exe)) continue;
                var index = merged.FindIndex(g => SameLibraryGame(g, cached));
                if (index < 0) merged.Add(cached);
                else
                {
                    var existing = merged[index];
                    var explicitChoice = cached.ExeOverride == true || cached.Source == "Manual";
                    var existingExplicit = existing.ExeOverride == true || existing.Source == "Manual";
                    if ((explicitChoice && !existingExplicit) ||
                        (explicitChoice == existingExplicit && KeepCachedExe(cached) && !KeepCachedExe(existing)))
                        merged[index] = cached;
                }
            }
            foreach (var incoming in scanned)
            {
                if (incoming == null || string.IsNullOrEmpty(incoming.Exe)) continue;
                var existing = merged.Find(g => SameLibraryGame(g, incoming));
                if (existing == null)
                {
                    incoming.ExeOverride = false;
                    merged.Add(incoming);
                    continue;
                }
                var samePath = string.Equals(existing.Exe, incoming.Exe, StringComparison.OrdinalIgnoreCase);
                if (!KeepCachedExe(existing))
                {
                    existing.Exe = incoming.Exe;
                    existing.ExeOverride = false;
                    samePath = true;
                }
                existing.Installed = samePath ? incoming.Installed :
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

        void InspectGame(GameInfo game)
        {
            if (_writeBusy) return;
            if (game == null || string.IsNullOrEmpty(game.Exe)) return;
            if (_session != null && _session.IsRunning)
            {
                if (_session.CanKill)
                {
                    var old = _session;
                    old.EventReceived -= OnEvent;
                    old.Completed -= OnCompleted;
                    old.Completed -= OnScanCompleted;
                    old.CancelInspect();
                    Detach(old);
                    _session = null;
                }
                else return;
            }

            _lastInspectResult = null;
            var fastProxy = PeProbe.QuickRecommend(game.Exe);
            if (!string.IsNullOrEmpty(fastProxy))
            {
                SelectProxy(fastProxy);
                if (_injectIcon != null) { _injectIcon.Text = "✓"; _injectIcon.Foreground = ReadyBrush; }
                if (_injectRecommendation != null)
                {
                    _injectRecommendation.Text = _language == "zh"
                        ? ("检测到程序导入 " + fastProxy + "，已自动选定为推荐代理。")
                        : ("Detected import for " + fastProxy + ", recommended proxy auto-selected.");
                }
            }

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

        string SelectMochizukiModels()
        {
            var dialog = CreateMochizukiModelDialog();
            return dialog.ShowDialog() == true ? dialog.Tag as string : null;
        }

        Window CreateMochizukiModelDialog()
        {
            var dialog = new Window
            {
                Owner = _window.IsVisible ? _window : null, Title = L("选择 Mochizuki 模型", "Select Mochizuki models"), Width = 500,
                SizeToContent = SizeToContent.Height, WindowStartupLocation = WindowStartupLocation.CenterOwner,
                ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false,
                Background = new SolidColorBrush(Color.FromRgb(32, 35, 43)), Foreground = Brushes.White
            };
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
            var values = new[] { "both", "small", "original" };
            var labels = new[] { L("两个模型（推荐）", "Both models (recommended)"), L("仅新模型", "New model only"), L("仅原始模型", "Original model only") };
            for (var i = 0; i < values.Length; i++)
            {
                var value = values[i];
                var radio = new RadioButton { Content = labels[i], Foreground = Brushes.White, GroupName = "MochizukiModels", Margin = new Thickness(0, 6, 0, 6) };
                radio.Checked += delegate { dialog.Tag = value; proceed.IsEnabled = true; };
                panel.Children.Add(radio);
            }
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            var cancel = new Button { Content = L("取消", "Cancel"), IsCancel = true, MinWidth = 100, Margin = new Thickness(4) };
            proceed.Click += delegate { dialog.DialogResult = true; };
            buttons.Children.Add(cancel);
            buttons.Children.Add(proceed);
            panel.Children.Add(buttons);
            dialog.Content = panel;
            return dialog;
        }

        Window CreateLmxxfModelDialog()
        {
            BackendInfo capability = null;
            if (_lastInspectResult != null && _lastInspectResult.Backends != null)
                foreach (var backend in _lastInspectResult.Backends)
                    if (backend != null && backend.Id == "lmxxf") { capability = backend; break; }
            var original = capability != null && capability.OriginalState == "ready";
            var small = capability != null && (capability.SmallState == "ready" || capability.SmallState == "preparable");
            var dialog = new Window {
                Owner = _window.IsVisible ? _window : null, Title = L("选择 lmxxf 模型", "Select lmxxf models"), Width = 540,
                SizeToContent = SizeToContent.Height, WindowStartupLocation = WindowStartupLocation.CenterOwner,
                ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false,
                Background = new SolidColorBrush(Color.FromRgb(32,35,43)), Foreground = Brushes.White
            };
            var panel = new StackPanel { Margin = new Thickness(24) };
            panel.Children.Add(new TextBlock { Text = L("请选择要安装的模型。现有的另一套模型会保留。", "Choose the models to install. Other installed models are retained."), TextWrapping = TextWrapping.Wrap });
            panel.Children.Add(new TextBlock { Text = "Original: " + (original ? L("就绪", "Ready") : L("缺失", "Missing")) + "  |  Small: " +
                (capability != null && capability.SmallState == "preparable" ? L("待准备", "Preparation required") : small ? L("就绪", "Ready") : L("缺失", "Missing")), Margin = new Thickness(0,10,0,10) });
            if (capability != null && !string.IsNullOrEmpty(BackendHintText(capability)))
                panel.Children.Add(new TextBlock { Text = BackendHintText(capability), TextWrapping = TextWrapping.Wrap, Foreground = Brushes.Orange, Margin = new Thickness(0,0,0,10) });
            var proceed = new Button { Content = L("继续", "Continue"), IsEnabled = false, MinWidth = 100, Margin = new Thickness(4) };
            var values = new[] { "both", "small", "original" };
            var labels = new[] { L("两个模型", "Both models"), "Small (310.9.16)", L("Original（原始模型）", "Original") };
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
            buttons.Children.Add(cancel); buttons.Children.Add(proceed); panel.Children.Add(buttons); dialog.Content = panel;
            return dialog;
        }

        string SelectLmxxfModels()
        {
            var dialog = CreateLmxxfModelDialog();
            return dialog.ShowDialog() == true ? dialog.Tag as string : null;
        }

        void ExecuteAction(string action, bool keepModelChoice = false)
        {
            if (_selectedGame == null || string.IsNullOrEmpty(_selectedGame.Exe)) return;
            if (_writeBusy || (_session != null && _session.IsRunning))
            {
                MessageBox.Show(_window, _language == "zh" ? "操作正在进行中…" : "An operation is already in progress.", _window.Title);
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
            if (action != "uninstall" && string.IsNullOrEmpty(proxy))
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

            if (action != "uninstall" && checkedBackends.Contains("lmxxf") &&
                (!keepModelChoice || string.IsNullOrEmpty(_decisions.LmxxfModels)))
            {
                _decisions.LmxxfModels = SelectLmxxfModels();
                if (string.IsNullOrEmpty(_decisions.LmxxfModels)) { ReleaseLock(); return; }
            }
            if (action != "uninstall" && checkedBackends.Contains("mochizuki") &&
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
                Backends = checkedBackends.ToArray(),
                ActiveBackend = _activeBackend,
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

                if (string.Equals(action, "inspect", StringComparison.OrdinalIgnoreCase))
                {
                    ApplyInspect(result);
                }
                else if (result != null && string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase))
                {
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
            }));
        }

        void ApplyInspect(LauncherResult result)
        {
            if (result == null) return;
            _lastInspectResult = result;
            var isZh = _language == "zh";

            if (!string.IsNullOrEmpty(result.Recommend))
            {
                if (_injectIcon != null) { _injectIcon.Text = "✓"; _injectIcon.Foreground = ReadyBrush; }
                if (_injectRecommendation != null)
                {
                    _injectRecommendation.Text = isZh
                        ? ("检测到程序导入 " + result.Recommend + "，已自动选定为推荐代理。")
                        : ("Detected import for " + result.Recommend + ", recommended proxy auto-selected.");
                }
                SelectProxy(result.Recommend);
            }
            else
            {
                if (_injectIcon != null) { _injectIcon.Text = "ℹ"; _injectIcon.Foreground = WarningBrush; }
                if (_injectRecommendation != null)
                {
                    _injectRecommendation.Text = isZh
                        ? "无法从导入表判断首选 DLL，建议优先尝试 dxgi.dll。"
                        : "Cannot determine preferred DLL, recommend trying dxgi.dll.";
                }
                if (string.IsNullOrEmpty(CurrentSelectedProxy))
                {
                    SelectProxy("dxgi.dll");
                }
            }

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
                    var pick = MessageBox.Show(_window, prompt, _window.Title, MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);
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
            _writeBusy = busy && action != "inspect" && action != "scan";
            if (_gameList != null) _gameList.IsEnabled = !_writeBusy;
            if (_packageBrowse != null) _packageBrowse.IsEnabled = !_writeBusy;
            if (_packageRootBox != null) _packageRootBox.IsEnabled = !_writeBusy;
            if (busy) _eventStage = action;
            if (_addButton != null) _addButton.IsEnabled = !busy;
            if (_changeExeButton != null) _changeExeButton.IsEnabled = !busy;
            if (_openFolderButton != null) _openFolderButton.IsEnabled = !busy;
            if (_refreshButton != null) _refreshButton.IsEnabled = !busy;
            if (_primaryButton != null) _primaryButton.IsEnabled = !busy;
            if (_uninstallButton != null) _uninstallButton.IsEnabled = !busy && (_selectedGame != null && _selectedGame.Installed);
            if (_proxyCombo != null) _proxyCombo.IsEnabled = !busy;
            if (_proxyPills != null)
            {
                foreach (var p in _proxyPills.Values) p.IsEnabled = !busy;
            }
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
            if (!e.Cancel) ReleaseLock();
        }
    }
}
