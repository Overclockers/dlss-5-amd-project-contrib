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
            if(_profileStatus!=null){
                var origin=_recommendations.UsingCache?L("缓存资料","Cached data"):L("随包资料","Bundled data");
                _profileStatus.Text=string.Format(L("{0}（{1} 款游戏）","{0} ({1} games)"),origin,_recommendations.Count);
                _profileStatus.ToolTip=string.Format(L("推荐规则来源：{0} · 共 {1} 款游戏 · 复核于 {2}","Recommendation source: {0} · {1} games · reviewed {2}"),origin,_recommendations.Count,_recommendations.Reviewed??"");
            }
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
            if(_statusText!=null)_statusText.Text=L("正在从 OptiScaler 官方 Wiki 同步推荐资料…","Syncing recommendations from the official OptiScaler Wiki…");
            if(_statusLed!=null)_statusLed.Fill=WarningBrush;
            try{
                var outcome=await _recommendations.RefreshAsync();
                if(_window.Dispatcher.HasShutdownStarted)return;
                // Do not revise a choice while an installation or confirmation is in progress.
                if(!_writeBusy && _targetMutex==null && _window.OwnedWindows.Count==0)UpdateProxyRecommendation();
                AppendLog("recommendations: "+outcome);
                ShowRecommendationOutcome(outcome);
            }finally{if(_refreshProfilesButton!=null)_refreshProfilesButton.IsEnabled=true;}
        }
        void ShowRecommendationOutcome(string outcome)
        {
            var colon=outcome.IndexOf(':');
            int count;
            int.TryParse(colon<0?"":outcome.Substring(colon+1),out count);
            var kind=colon<0?outcome:outcome.Substring(0,colon);
            switch(kind){
                case "updated":
                    if(_statusText!=null)_statusText.Text=string.Format(L("已从 OptiScaler 官方 Wiki 同步 {0} 条游戏推荐规则。","Synced {0} game recommendation rules from the official OptiScaler Wiki."),count);
                    if(_statusLed!=null)_statusLed.Fill=ReadyBrush;
                    break;
                case "not-modified":
                    if(_statusText!=null)_statusText.Text=string.Format(L("推荐资料已是最新（{0} 条规则）。","Recommendations are already up to date ({0} rules)."),count);
                    if(_statusLed!=null)_statusLed.Fill=ReadyBrush;
                    break;
                case "offline":
                    if(_statusText!=null)_statusText.Text=L("无法连接 OptiScaler Wiki，继续使用本地推荐资料。","Could not reach the OptiScaler Wiki; keeping local recommendations.");
                    if(_statusLed!=null)_statusLed.Fill=WarningBrush;
                    break;
                case "timeout":
                    if(_statusText!=null)_statusText.Text=L("连接 OptiScaler Wiki 超时，继续使用本地推荐资料。","Timed out contacting the OptiScaler Wiki; keeping local recommendations.");
                    if(_statusLed!=null)_statusLed.Fill=WarningBrush;
                    break;
                case "invalid":
                    if(_statusText!=null)_statusText.Text=L("OptiScaler Wiki 返回无法解析的内容，继续使用本地推荐资料。","The OptiScaler Wiki returned unusable content; keeping local recommendations.");
                    if(_statusLed!=null)_statusLed.Fill=WarningBrush;
                    break;
                case "busy":
                    if(_statusText!=null)_statusText.Text=L("正在同步推荐资料，请稍候。","Recommendation sync is already running.");
                    break;
            }
        }
    }
}
