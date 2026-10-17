using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Threading;

namespace OptScalerNR.Launcher
{
    public sealed class WorkerSession : IDisposable
    {
        readonly Process _process;
        readonly FileStream _events;
        readonly StreamReader _reader;
        readonly DispatcherTimer _timer;
        readonly string _resultPath;
        long _offset;
        bool _done;

        public event Action<string, string> EventReceived;
        public event Action<WorkerSession, LauncherResult, int> Completed;

        public bool IsRunning { get { return _process != null && !_process.HasExited; } }
        public bool CanKill { get; private set; }

        WorkerSession(Process process, string eventsPath, string resultPath, bool canKill)
        {
            _process = process;
            _resultPath = resultPath;
            CanKill = canKill;
            _events = new FileStream(eventsPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            _reader = new StreamReader(_events, Encoding.UTF8);
            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
            _timer.Tick += OnTick;
            _timer.Start();
        }

        public static WorkerSession Start(string workerScript, LauncherRequest request, string workDir, bool canKill)
        {
            Directory.CreateDirectory(workDir);
            var reqPath = Path.Combine(workDir, "request.json");
            var eventsPath = Path.Combine(workDir, "events.jsonl");
            var resultPath = Path.Combine(workDir, "result.json");
            if (File.Exists(eventsPath)) File.Delete(eventsPath);
            if (File.Exists(resultPath)) File.Delete(resultPath);
            File.WriteAllText(eventsPath, "", new UTF8Encoding(false));
            JsonUtil.WriteFile(reqPath, request);

            var psi = new ProcessStartInfo();
            psi.FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                @"WindowsPowerShell\v1.0\powershell.exe");
            psi.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                workerScript + "\" -RequestJson \"" + reqPath + "\" -EventsJsonl \"" +
                eventsPath + "\" -ResultJson \"" + resultPath + "\"";
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.StandardOutputEncoding = Encoding.UTF8;
            psi.StandardErrorEncoding = Encoding.UTF8;
            var proc = Process.Start(psi);
            if (proc == null) throw new InvalidOperationException("failed to start worker");
            return new WorkerSession(proc, eventsPath, resultPath, canKill);
        }

        void OnTick(object sender, EventArgs e)
        {
            DrainEvents();
            if (_done || !_process.HasExited) return;
            _done = true;
            _timer.Stop();
            DrainEvents();
            LauncherResult result = null;
            try
            {
                if (File.Exists(_resultPath))
                    result = JsonUtil.Read<LauncherResult>(File.ReadAllText(_resultPath, Encoding.UTF8));
            }
            catch (Exception ex)
            {
                result = new LauncherResult { Status = "failed", Error = ex.Message };
            }
            if (result == null)
                result = new LauncherResult { Status = "failed", Error = "worker exited without a result file" };
            var code = _process.ExitCode;
            if (code == 0 && !string.Equals(result.Status, "success", StringComparison.OrdinalIgnoreCase))
                result.Status = result.Status ?? "failed";
            if (Completed != null) Completed(this, result, code);
        }

        void DrainEvents()
        {
            _events.Seek(_offset, SeekOrigin.Begin);
            string line;
            while ((line = _reader.ReadLine()) != null)
            {
                _offset = _events.Position;
                if (line.Length == 0) continue;
                try
                {
                    var ev = JsonUtil.Read<EventLine>(line);
                    if (EventReceived != null) EventReceived(ev.Stage, ev.Message);
                }
                catch
                {
                    if (EventReceived != null) EventReceived("log", line);
                }
            }
            _offset = _events.Position;
        }

        public void CancelInspect()
        {
            if (!CanKill || _process.HasExited) return;
            try { _process.Kill(); } catch { }
        }

        public void Dispose()
        {
            _timer.Stop();
            _reader.Dispose();
            _events.Dispose();
            if (_process != null)
            {
                try { _process.Dispose(); } catch { }
            }
        }

        [System.Runtime.Serialization.DataContract]
        sealed class EventLine
        {
            [System.Runtime.Serialization.DataMember(Name = "stage")] public string Stage = "";
            [System.Runtime.Serialization.DataMember(Name = "message")] public string Message = "";
        }
    }
}
