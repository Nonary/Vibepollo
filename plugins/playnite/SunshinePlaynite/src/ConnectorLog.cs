using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using Playnite.SDK;

namespace SunshinePlaynite
{
    internal sealed class ConnectorLog : IDisposable
    {
        private readonly object sync = new object();
        private readonly ILogger playniteLog;
        private readonly string path;
        private int debugEnabled;

        public ConnectorLog(ILogger playniteLog, bool enableDebugLogging)
        {
            this.playniteLog = playniteLog;
            debugEnabled = enableDebugLogging ? 1 : 0;
            var root = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            var directory = Path.Combine(root, "Sunshine", "logs");
            Directory.CreateDirectory(directory);
            path = Path.Combine(directory, "sunshine_playnite-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + ".log");
            Purge(directory);
            Info("=== Sunshine Playnite Connector starting ===");
        }

        public void Debug(string message)
        {
            if (Volatile.Read(ref debugEnabled) != 0) Write("DEBUG", message);
        }
        public void Info(string message) { Write("INFO", message); }
        public void Warn(string message) { Write("WARN", message); }
        public void Error(string message) { Write("ERROR", message); }

        public void SetDebugEnabled(bool enabled)
        {
            Volatile.Write(ref debugEnabled, enabled ? 1 : 0);
        }

        private void Write(string level, string message)
        {
            var line = string.Format("[{0:yyyy-MM-dd HH:mm:ss.fff}] [{1}] [T#{2}] {3}",
                DateTime.Now, level, System.Threading.Thread.CurrentThread.ManagedThreadId, message);
            lock (sync)
            {
                try { File.AppendAllText(path, line + Environment.NewLine, Encoding.UTF8); } catch { }
            }
            try
            {
                if (level == "ERROR") playniteLog.Error(message);
                else if (level == "WARN") playniteLog.Warn(message);
                else if (level == "DEBUG") playniteLog.Debug(message);
                else playniteLog.Info(message);
            }
            catch { }
        }

        private static void Purge(string directory)
        {
            try
            {
                foreach (var file in new DirectoryInfo(directory).GetFiles("sunshine_playnite-*.log*")
                    .OrderByDescending(x => x.CreationTimeUtc).Skip(30))
                {
                    try { file.Delete(); } catch { }
                }
            }
            catch { }
        }

        public void Dispose() { }
    }
}
