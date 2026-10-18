using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        ProxyRecommendations _recommendations;
        readonly Dictionary<string,string> _proxyChoices=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        bool _selectingProxy;
        Button _refreshProfilesButton,_useRecommendationButton;
        TextBlock _profileStatus;
        void InitRecommendations()
        {
            _recommendations=new ProxyRecommendations(_settingsDir);
            _refreshProfilesButton=(Button)_window.FindName("RefreshProfilesButton");
            _useRecommendationButton=(Button)_window.FindName("UseRecommendationButton");
            _profileStatus=(TextBlock)_window.FindName("ProfileStatus");
            if(_refreshProfilesButton!=null)_refreshProfilesButton.Click+=(s,e)=>RefreshProfiles();
            if(_useRecommendationButton!=null)_useRecommendationButton.Click+=delegate{
                if(_selectedGame==null)return;_proxyChoices.Remove(_selectedGame.Id);SaveSettings();UpdateProxyRecommendation();
            };
        }
        void RememberProxyChoice()
        {
            if(_selectingProxy||_loadingSettings||_applyingLanguage||_selectedGame==null||string.IsNullOrEmpty(_selectedGame.Id))return;
            var proxy=CurrentSelectedProxy;
            if(ProxyRecommendations.Supports(proxy)){_proxyChoices[_selectedGame.Id]=proxy;SaveSettings();UpdateProxyRecommendation();}
        }
        void UpdateProxyRecommendation(string quickCandidate=null)
        {
            if(_recommendations==null)return;
            if(_refreshProfilesButton!=null)_refreshProfilesButton.Content=L("更新推荐资料","Refresh recommendations");
            if(_useRecommendationButton!=null){_useRecommendationButton.Content=L("使用推荐","Use suggestion");_useRecommendationButton.IsEnabled=_selectedGame!=null&&!_writeBusy;}
            if(_profileStatus!=null)_profileStatus.Text=(_recommendations.UsingCache?L("本地缓存资料 · ","Cached data · "):L("随包资料 · ","Bundled data · "))+_recommendations.Reviewed;
            if(_selectedGame==null)return;
            var profile=_recommendations.Match(_selectedGame);
            string manual;
            var hasManual=_proxyChoices.TryGetValue(_selectedGame.Id,out manual)&&ProxyRecommendations.Supports(manual);
            var guess=_lastInspectResult!=null?_lastInspectResult.Recommend:quickCandidate;
            var proxy=hasManual?manual:profile!=null?profile.Preferred:guess;
            SelectProxy(proxy);
            if(_injectRecommendation!=null){
                if(hasManual)_injectRecommendation.Text=L("使用你的手选代理：","Using your selected proxy: ")+manual;
                else if(profile!=null)_injectRecommendation.Text=L("Wiki 游戏规则建议：","Wiki game rule suggests: ")+profile.Preferred+
                    (profile.Alternatives.Length>0?L("；备选 ","; alternatives ")+string.Join(", ",profile.Alternatives):"");
                else if(!string.IsNullOrEmpty(guess))_injectRecommendation.Text=L("本地检测候选（未验证注入）：","Local candidate (injection not verified): ")+guess;
                else _injectRecommendation.Text=L("尚无已验证的游戏规则，也未找到可靠导入候选。请手动选择代理。","No reviewed game rule or reliable import candidate was found. Select a proxy manually.");
                if(profile!=null)_injectRecommendation.Text+="\n"+(_language=="zh"?profile.NotesZh:profile.NotesEn)+"\n"+profile.Source;
            }
            if(_injectIcon!=null){_injectIcon.Text=hasManual||profile!=null?"✓":"ℹ";_injectIcon.Foreground=hasManual||profile!=null?ReadyBrush:WarningBrush;}
        }
        async void RefreshProfiles()
        {
            if(_recommendations==null)return;
            if(_refreshProfilesButton!=null)_refreshProfilesButton.IsEnabled=false;
            try{
                var outcome=await _recommendations.RefreshAsync();
                if(_window.Dispatcher.HasShutdownStarted)return;
                // Do not revise a choice while an installation or confirmation is in progress.
                if(!_writeBusy && _targetMutex==null && _window.OwnedWindows.Count==0)UpdateProxyRecommendation();
                AppendLog("recommendations: "+outcome);
            }finally{if(_refreshProfilesButton!=null)_refreshProfilesButton.IsEnabled=true;}
        }
    }
}
