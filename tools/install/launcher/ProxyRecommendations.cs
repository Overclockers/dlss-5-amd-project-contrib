using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OptScalerNR.Launcher
{
    [DataContract]
    public sealed class ProxyProfileDocument
    {
        [DataMember(Name="schema")] public int Schema;
        [DataMember(Name="revision")] public long Revision;
        [DataMember(Name="reviewed")] public string Reviewed;
        [DataMember(Name="proxies")] public string[] Proxies;
        [DataMember(Name="profiles")] public ProxyProfile[] Profiles;
    }
    [DataContract]
    public sealed class ProxyProfile
    {
        [DataMember(Name="id")] public string Id;
        [DataMember(Name="names")] public string[] Names;
        [DataMember(Name="pathSegments")] public string[] PathSegments;
        [DataMember(Name="preferred")] public string Preferred;
        [DataMember(Name="alternatives")] public string[] Alternatives;
        [DataMember(Name="notesEn")] public string NotesEn;
        [DataMember(Name="notesZh")] public string NotesZh;
        [DataMember(Name="source")] public string Source;
        [DataMember(Name="sourceFormat")] public string SourceFormat;
        [DataMember(Name="upstreamVersion")] public string UpstreamVersion;
    }

    public sealed class ProxyRecommendations : IDisposable
    {
        const int Limit = 256 * 1024;
        public const string UpdateUrl = "https://raw.githubusercontent.com/TheAutomatic/dlss-5-amd-project/main/assets/compatibility/proxy-profiles.json";
        static readonly ProxyProfileDocument Seed = ReadSeed();
        public static readonly string[] ProxyNames = Seed.Proxies;
        readonly object _gate = new object();
        readonly string _directory;
        ProxyProfileDocument _current = Seed;
        string _etag;
        bool _cached, _disposed;
        int _updating;
        HttpWebRequest _request;

        static ProxyProfileDocument ReadSeed()
        {
            using (var stream = typeof(ProxyRecommendations).Assembly.GetManifestResourceStream("OptScalerNR.Launcher.proxy-profiles.json"))
            using (var reader = new StreamReader(stream, Encoding.UTF8))
                return JsonUtil.Read<ProxyProfileDocument>(reader.ReadToEnd());
        }
        public ProxyRecommendations(string settingsDirectory)
        {
            Validate(Seed);
            _directory = Path.Combine(settingsDirectory, "compatibility");
            try {
                var path = Path.Combine(_directory, "proxy-profiles.json");
                if (File.Exists(path) && new FileInfo(path).Length <= Limit) {
                    var doc = JsonUtil.Read<ProxyProfileDocument>(File.ReadAllText(path, Encoding.UTF8));
                    Validate(doc);
                    if (doc.Revision >= Seed.Revision) { _current = doc; _cached = true; }
                    var etag = Path.Combine(_directory, "etag.txt");
                    if (_cached && File.Exists(etag) && new FileInfo(etag).Length < 512) {
                        var value = File.ReadAllText(etag);
                        if (value.IndexOfAny(new[] {'\r','\n'}) < 0) _etag = value;
                    }
                }
            } catch { /* Invalid cached data never replaces the bundled document. */ }
        }
        public string Reviewed { get { lock (_gate) return _current.Reviewed; } }
        public bool UsingCache { get { lock (_gate) return _cached; } }
        public static bool Supports(string name) { return Array.Exists(ProxyNames, p => string.Equals(p, name, StringComparison.OrdinalIgnoreCase)); }

        static void Validate(ProxyProfileDocument doc)
        {
            DateTime reviewed;
            if (doc == null || doc.Schema != 1 || doc.Revision < 1 || doc.Profiles == null || doc.Profiles.Length > 1500 ||
                !DateTime.TryParseExact(doc.Reviewed, "yyyy-MM-dd", System.Globalization.CultureInfo.InvariantCulture,
                    System.Globalization.DateTimeStyles.None, out reviewed) || doc.Proxies == null || doc.Proxies.Length != ProxyNames.Length)
                throw new InvalidDataException("Invalid recommendation document.");
            var proxies = new HashSet<string>(doc.Proxies, StringComparer.OrdinalIgnoreCase);
            foreach (var name in ProxyNames) if (!proxies.Contains(name)) throw new InvalidDataException("Unsupported proxy list.");
            var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var p in doc.Profiles) {
                Uri uri;
                if (p == null || string.IsNullOrEmpty(p.Id) || !ids.Add(p.Id) || !Supports(p.Preferred) ||
                    p.Names == null || p.Names.Length > 32 || p.PathSegments == null || p.PathSegments.Length > 32 ||
                    p.Alternatives == null || p.Alternatives.Length > ProxyNames.Length ||
                    !Uri.TryCreate(p.Source, UriKind.Absolute, out uri) || uri.Scheme != "https" || uri.Host != "github.com" ||
                    (p.NotesEn ?? "").Length > 3000 || (p.NotesZh ?? "").Length > 3000)
                    throw new InvalidDataException("Invalid recommendation record.");
                foreach (var a in p.Alternatives) if (!Supports(a)) throw new InvalidDataException("Unsupported proxy.");
                foreach (var n in p.Names) if (string.IsNullOrWhiteSpace(n) || n.Length > 200) throw new InvalidDataException("Invalid game identity.");
                foreach (var n in p.PathSegments) if (string.IsNullOrWhiteSpace(n) || n.Length > 200 || n.IndexOfAny(new[] {'/','\\'}) >= 0)
                    throw new InvalidDataException("Invalid game path identity.");
                if (p.Names.Length == 0 && p.PathSegments.Length == 0) throw new InvalidDataException("Missing game identity.");
            }
        }
        public ProxyProfile Match(GameInfo game)
        {
            if (game == null) return null;
            ProxyProfileDocument doc;
            lock (_gate) doc = _current;
            var paths = ((game.Root ?? "") + "\\" + (game.Exe ?? "")).Split(new[] {'\\','/'}, StringSplitOptions.RemoveEmptyEntries);
            foreach (var profile in doc.Profiles) {
                foreach (var name in profile.Names) if (string.Equals(game.Name, name, StringComparison.OrdinalIgnoreCase)) return profile;
                foreach (var segment in profile.PathSegments)
                    foreach (var actual in paths) if (string.Equals(actual, segment, StringComparison.OrdinalIgnoreCase)) return profile;
            }
            return null;
        }

        public async Task<string> RefreshAsync()
        {
            if (_disposed || Interlocked.Exchange(ref _updating, 1) != 0) return "busy";
            var timer = Stopwatch.StartNew();
            var deadline = new CancellationTokenSource();
            var request = (HttpWebRequest)WebRequest.Create(UpdateUrl);
            request.UserAgent = "OptScalerNR-Launcher/1";
            request.Timeout = request.ReadWriteTimeout = 10000;
            request.AllowAutoRedirect = false;
            if (!string.IsNullOrEmpty(_etag)) request.Headers[HttpRequestHeader.IfNoneMatch] = _etag;
            lock (_gate) {
                if (_disposed) { Interlocked.Exchange(ref _updating, 0); deadline.Dispose(); return "cancelled"; }
                _request = request;
            }
            var work = Task.Run(() => Download(request, deadline.Token, timer));
            try {
                var completed = await Task.WhenAny(work, Task.Delay(10000)).ConfigureAwait(false);
                if (completed != work) { deadline.Cancel(); request.Abort(); return "timeout"; }
                return await work.ConfigureAwait(false);
            } finally {
                deadline.Cancel();
                lock (_gate) if (ReferenceEquals(_request, request)) _request = null;
                Interlocked.Exchange(ref _updating, 0);
                // The token remains valid for a DNS request that completes after cancellation.
                ReleaseWhenDone(work, deadline);
            }
        }
        static void ReleaseWhenDone(Task work, CancellationTokenSource source)
        { work.ContinueWith(t => { var ignored = t.Exception; source.Dispose(); }, TaskScheduler.Default); }
        string Download(HttpWebRequest request, CancellationToken token, Stopwatch timer)
        {
            try {
                using (var response = (HttpWebResponse)request.GetResponse()) {
                    if (response.StatusCode == HttpStatusCode.NotModified) return "unchanged";
                    if (response.StatusCode != HttpStatusCode.OK || response.ContentLength > Limit) return "invalid";
                    using (var input = response.GetResponseStream())
                    using (var output = new MemoryStream()) {
                        var buffer = new byte[8192]; int read;
                        while ((read = input.Read(buffer, 0, buffer.Length)) > 0) {
                            if (token.IsCancellationRequested || timer.ElapsedMilliseconds >= 10000) return "timeout";
                            if (output.Length + read > Limit) return "invalid";
                            output.Write(buffer, 0, read);
                        }
                        var text = new UTF8Encoding(false, true).GetString(output.ToArray());
                        var doc = JsonUtil.Read<ProxyProfileDocument>(text); Validate(doc);
                        lock (_gate) {
                            if (_disposed || token.IsCancellationRequested || timer.ElapsedMilliseconds >= 10000) return "timeout";
                            if (doc.Revision < _current.Revision) return "unchanged";
                        }
                        // File I/O must not hold the lock used by UI readers. Serialize writers
                        // across launcher windows and never replace a newer cache with an older response.
                        string mutexName;
                        using (var sha = SHA256.Create()) mutexName = "Local\\OptScalerNR.Profiles." +
                            BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(_directory).ToLowerInvariant()))).Replace("-", "");
                        using (var mutex = new Mutex(false, mutexName)) {
                            bool acquired;
                            try { acquired = mutex.WaitOne(0); } catch (AbandonedMutexException) { acquired = true; }
                            if (!acquired) return "busy";
                            try {
                            if (token.IsCancellationRequested || timer.ElapsedMilliseconds >= 10000) return "timeout";
                            Directory.CreateDirectory(_directory);
                            var path = Path.Combine(_directory, "proxy-profiles.json");
                            if (File.Exists(path) && new FileInfo(path).Length <= Limit) {
                                try {
                                    var existing = JsonUtil.Read<ProxyProfileDocument>(File.ReadAllText(path, Encoding.UTF8));
                                    Validate(existing);
                                    if (existing.Revision > doc.Revision) return "unchanged";
                                } catch { /* A damaged old file can be replaced by validated data. */ }
                            }
                            var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                            try {
                                File.WriteAllText(temporary, text, new UTF8Encoding(false));
                                if (token.IsCancellationRequested || timer.ElapsedMilliseconds >= 10000) return "timeout";
                                if (File.Exists(path)) File.Replace(temporary, path, null); else File.Move(temporary, path);
                            } finally { if (File.Exists(temporary)) File.Delete(temporary); }
                            var etag = response.Headers[HttpResponseHeader.ETag];
                            if (!string.IsNullOrEmpty(etag) && etag.Length < 512) File.WriteAllText(Path.Combine(_directory, "etag.txt"), etag);
                            lock (_gate) {
                                if (_disposed || token.IsCancellationRequested || timer.ElapsedMilliseconds >= 10000) return "timeout";
                                _etag = etag; _current = doc; _cached = true;
                            }
                            } finally { mutex.ReleaseMutex(); }
                        }
                        return "updated";
                    }
                }
            } catch (WebException ex) {
                var response = ex.Response as HttpWebResponse;
                if (response != null) { using (response) if (response.StatusCode == HttpStatusCode.NotModified) return "unchanged"; }
                return token.IsCancellationRequested ? "timeout" : "offline";
            } catch { return "invalid"; }
        }
        public void Dispose()
        {
            lock (_gate) { _disposed = true; if (_request != null) _request.Abort(); }
        }
    }
}
