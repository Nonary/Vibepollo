using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Playnite.SDK;
using Playnite.SDK.Models;

namespace SunshinePlaynite
{
    internal sealed class ConnectorService : IDisposable
    {
        private const string ControlPipeName = "Sunshine.PlayniteExtension";
        private const int DataConnectionTimeoutMs = 5000;
        private const int HelloTimeoutMs = 5000;
        private const int ShutdownFlushTimeoutMs = 500;
        private readonly IPlayniteAPI api;
        private readonly ConnectorLog log;
        private readonly PlayniteDataMapper dataMapper;
        private readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        private readonly object lifecycleLock = new object();
        private readonly object coreLock = new object();
        private readonly object gameStateLock = new object();
        private readonly object snapshotLock = new object();
        private readonly ConcurrentDictionary<string, PipeConnection> launchers = new ConcurrentDictionary<string, PipeConnection>();
        private readonly ConcurrentDictionary<Guid, byte> sunshineGames = new ConcurrentDictionary<Guid, byte>();
        private readonly ConcurrentDictionary<Guid, byte> pendingGames = new ConcurrentDictionary<Guid, byte>();
        private readonly EnvironmentScopes environmentScopes = new EnvironmentScopes();
        private CancellationTokenSource cancellation;
        private Task serverTask;
        private PipeConnection core;
        private NamedPipeServerStream pendingPipe;
        private Timer snapshotTimer;
        private int started;
        private int libraryNotificationsEnabled = 1;

        public ConnectorService(IPlayniteAPI api, ILogger logger, bool enableDebugLogging)
        {
            this.api = api;
            log = new ConnectorLog(logger, enableDebugLogging);
            dataMapper = new PlayniteDataMapper(api, Serialize);
        }

        public void ApplySettings(bool notifyLibraryChanges, bool enableDebugLogging)
        {
            Volatile.Write(ref libraryNotificationsEnabled, notifyLibraryChanges ? 1 : 0);
            log.SetDebugEnabled(enableDebugLogging);
        }

        public void Start()
        {
            lock (lifecycleLock)
            {
                if (Volatile.Read(ref started) != 0) return;
                var nextCancellation = new CancellationTokenSource();
                try
                {
                    cancellation = nextCancellation;
                    api.Database.Games.ItemCollectionChanged += GamesChanged;
                    api.Database.Games.ItemUpdated += GamesUpdated;
                    snapshotTimer = new Timer(_ => SendSnapshot(), null, Timeout.Infinite, Timeout.Infinite);
                    serverTask = Task.Factory.StartNew(() => ServerLoop(nextCancellation.Token), nextCancellation.Token,
                        TaskCreationOptions.LongRunning, TaskScheduler.Default);
                    Volatile.Write(ref started, 1);
                    log.Info("Compiled Playnite plugin started");
                }
                catch
                {
                    try { api.Database.Games.ItemCollectionChanged -= GamesChanged; } catch { }
                    try { api.Database.Games.ItemUpdated -= GamesUpdated; } catch { }
                    try { if (snapshotTimer != null) snapshotTimer.Dispose(); } catch { }
                    snapshotTimer = null;
                    serverTask = null;
                    cancellation = null;
                    nextCancellation.Dispose();
                    throw;
                }
            }
        }

        public void Stop()
        {
            lock (lifecycleLock)
            {
                if (Volatile.Read(ref started) == 0) return;
                Volatile.Write(ref started, 0);
                log.Info("Beginning shutdown");
                try
                {
                    try { SendShutdownHandoff(); }
                    catch (Exception ex) { log.Warn("Shutdown handoff failed: " + ex.Message); }
                    try { FlushConnections(ShutdownFlushTimeoutMs); }
                    catch (Exception ex) { log.Warn("Shutdown flush failed: " + ex.Message); }
                }
                finally
                {
                    try { api.Database.Games.ItemCollectionChanged -= GamesChanged; } catch { }
                    try { api.Database.Games.ItemUpdated -= GamesUpdated; } catch { }

                    var timer = snapshotTimer;
                    snapshotTimer = null;
                    try { if (timer != null) timer.Dispose(); } catch { }

                    var stopCancellation = cancellation;
                    try { if (stopCancellation != null) stopCancellation.Cancel(); } catch { }
                    try { if (pendingPipe != null) pendingPipe.Dispose(); } catch { }
                    pendingPipe = null;
                    try { ReplaceCore(null); } catch { }
                    foreach (var item in launchers.ToArray())
                    {
                        PipeConnection ignored;
                        if (launchers.TryRemove(item.Key, out ignored))
                        {
                            try { ignored.Dispose(); } catch { }
                        }
                        try { environmentScopes.Clear(item.Key); } catch { }
                    }
                    lock (gameStateLock)
                    {
                        pendingGames.Clear();
                        sunshineGames.Clear();
                    }

                    var task = serverTask;
                    serverTask = null;
                    try { if (task != null) task.Wait(1000); } catch { }
                    cancellation = null;
                    try { if (stopCancellation != null) stopCancellation.Dispose(); } catch { }
                    log.Info("Connector stopped");
                }
            }
        }

        public void Dispose()
        {
            Stop();
            log.Dispose();
        }

        public void GameStarted(Game game)
        {
            if (game == null) return;
            log.Info("Game started: " + game.Name + " [" + game.Id + "]");
            var payload = RunOnUi(() => dataMapper.BuildStatus("gameStarted", game), true);
            lock (gameStateLock)
            {
                if (launchers.Values.Any(x => SameId(x.GameId, game.Id))) sunshineGames.TryAdd(game.Id, 0);
                SendCore(payload);
                Broadcast(payload, null);
            }
        }

        public void GameStopped(Game game)
        {
            if (game == null) return;
            log.Info("Game stopped: " + game.Name + " [" + game.Id + "]");
            var payload = RunOnUi(() => dataMapper.BuildStatus("gameStopped", game), true);
            lock (gameStateLock)
            {
                byte ignored;
                pendingGames.TryRemove(game.Id, out ignored);
                if (!sunshineGames.TryRemove(game.Id, out ignored))
                {
                    log.Debug("Ignoring stop status for an untracked game: " + game.Id);
                    return;
                }
                SendCore(payload);
                Broadcast(payload, null);
            }
        }

        public void QueueSnapshot()
        {
            var timer = snapshotTimer;
            if (timer != null) timer.Change(3000, Timeout.Infinite);
        }

        private void GamesChanged(object sender, ItemCollectionChangedEventArgs<Game> args)
        {
            if (Volatile.Read(ref libraryNotificationsEnabled) != 0) QueueSnapshot();
        }

        private void GamesUpdated(object sender, ItemUpdatedEventArgs<Game> args)
        {
            if (Volatile.Read(ref libraryNotificationsEnabled) != 0) QueueSnapshot();
        }

        private void ServerLoop(CancellationToken token)
        {
            log.Info("Pipe server starting");
            var security = ConnectorPipeTransport.CreatePipeSecurity();
            while (!token.IsCancellationRequested)
            {
                NamedPipeServerStream control = null;
                NamedPipeServerStream data = null;
                try
                {
                    control = ConnectorPipeTransport.CreateServer(ControlPipeName, security);
                    pendingPipe = control;
                    ConnectorPipeTransport.WaitForConnection(control, token);
                    var pipeName = Guid.NewGuid().ToString("B").ToUpperInvariant();
                    data = ConnectorPipeTransport.CreateServer(pipeName, security);
                    ConnectorPipeTransport.WriteHandshake(control, pipeName);
                    if (!ConnectorPipeTransport.WaitForAck(control)) throw new IOException("Handshake ACK missing");
                    control.Dispose();
                    control = null;
                    pendingPipe = data;
                    ConnectorPipeTransport.WaitForConnection(data, token, DataConnectionTimeoutMs);

                    var reader = new StreamReader(data, new UTF8Encoding(false), false, 8192, true);
                    var writer = new StreamWriter(data, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
                    var helloLine = ConnectorPipeTransport.ReadLine(reader, token, HelloTimeoutMs);
                    if (helloLine == null) throw new IOException("No hello received");
                    var hello = ParseObject(helloLine);
                    var role = ConnectorPipeTransport.ValidateClient(data, hello);
                    var connection = new PipeConnection(pipeName, data, reader, writer, log);
                    data = null;
                    if (string.Equals(role, "sunshine", StringComparison.OrdinalIgnoreCase))
                    {
                        ReplaceCore(connection);
                        StartCore(connection, token);
                    }
                    else
                    {
                        connection.Pid = GetInt(hello, "pid");
                        connection.GameId = GetString(hello, "gameId");
                        launchers[pipeName] = connection;
                        StartLauncher(connection, token);
                    }
                }
                catch (OperationCanceledException) { break; }
                catch (TimeoutException ex)
                {
                    if (!token.IsCancellationRequested) log.Warn("Pipe handshake timed out: " + ex.Message);
                }
                catch (Exception ex)
                {
                    if (!token.IsCancellationRequested) log.Warn("Pipe connection failed: " + ex.Message);
                }
                finally
                {
                    pendingPipe = null;
                    if (control != null) control.Dispose();
                    if (data != null) data.Dispose();
                }
            }
            log.Info("Pipe server exiting");
        }

        private void StartCore(PipeConnection connection, CancellationToken token)
        {
            log.Info("Sunshine core connection accepted");
            Task.Factory.StartNew(() =>
            {
                try
                {
                    SendSnapshot();
                    SendRunningGames();
                    ReadCore(connection, token);
                }
                finally
                {
                    lock (coreLock)
                    {
                        if (ReferenceEquals(core, connection)) core = null;
                    }
                    connection.Dispose();
                }
            }, token, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        private void ReadCore(PipeConnection connection, CancellationToken token)
        {
            while (!token.IsCancellationRequested && ReferenceEquals(GetCore(), connection))
            {
                var line = connection.Reader.ReadLine();
                if (line == null) break;
                try { HandleCoreCommand(ParseObject(line)); }
                catch (Exception ex) { log.Warn("Failed to handle Sunshine command: " + ex.Message); }
            }
        }

        private void HandleCoreCommand(Dictionary<string, object> message)
        {
            if (GetString(message, "type") != "command") return;
            var command = GetString(message, "command");
            var id = GetString(message, "id");
            if (command == "launch" && Guid.TryParse(id, out var gameId))
            {
                lock (gameStateLock) sunshineGames.TryAdd(gameId, 0);
                var environment = GetEnvironment(message);
                RunOnUi(() => environmentScopes.RunTemporary(environment, () => api.StartGame(gameId)), false);
            }
            else if (command == "stop")
            {
                SendStopRequested(id);
            }
            else if (command == "snapshot")
            {
                SendSnapshot();
            }
            else if (command == "set-cover")
            {
                var success = false;
                var error = string.Empty;
                try
                {
                    var path = GetString(message, "path");
                    if (!Guid.TryParse(id, out gameId) || !File.Exists(path)) throw new IOException("Invalid game ID or cover path");
                    success = RunOnUi(() => dataMapper.SetCover(gameId, path), true);
                    if (!success) throw new InvalidOperationException("Playnite rejected the cover metadata update");
                }
                catch (Exception ex) { error = ex.Message; }
                SendCore(new Dictionary<string, object>
                {
                    ["type"] = "commandResult", ["command"] = "set-cover",
                    ["requestId"] = GetString(message, "requestId"), ["success"] = success, ["error"] = error
                });
            }
        }

        private void StartLauncher(PipeConnection connection, CancellationToken token)
        {
            log.Info(string.Format("Launcher connection accepted id={0} pid={1}", connection.Id, connection.Pid));
            SyncLauncher(connection);
            FlushPending(connection);
            Task.Factory.StartNew(() =>
            {
                try
                {
                    while (!token.IsCancellationRequested)
                    {
                        var line = connection.Reader.ReadLine();
                        if (line == null) break;
                        var message = ParseObject(line);
                        if (GetString(message, "type") != "command") continue;
                        var command = GetString(message, "command");
                        if (command == "launch" && Guid.TryParse(GetString(message, "id"), out var gameId))
                        {
                            lock (gameStateLock) sunshineGames.TryAdd(gameId, 0);
                            environmentScopes.Set(connection.Id, GetEnvironment(message));
                            RunOnUi(() => api.StartGame(gameId), false);
                        }
                        else if (command == "set-environment")
                        {
                            environmentScopes.Set(connection.Id, GetEnvironment(message));
                        }
                    }
                }
                catch (Exception ex) { if (!token.IsCancellationRequested) log.Warn("Launcher reader failed: " + ex.Message); }
                finally
                {
                    PipeConnection ignored;
                    launchers.TryRemove(connection.Id, out ignored);
                    environmentScopes.Clear(connection.Id);
                    connection.Dispose();
                    log.Info("Launcher disconnected: " + connection.Id);
                }
            }, token, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        private void SendSnapshot()
        {
            lock (snapshotLock)
            {
                var target = GetCore();
                if (target == null) return;
                try
                {
                    var snapshot = RunOnUi(() => dataMapper.BuildSnapshot(), true);
                    var messages = new List<string>
                    {
                        Serialize(new Dictionary<string, object> { ["type"] = "snapshotStart" }),
                        Serialize(new Dictionary<string, object> { ["type"] = "plugins", ["payload"] = snapshot.Plugins }),
                        Serialize(new Dictionary<string, object> { ["type"] = "categories", ["payload"] = snapshot.Categories })
                    };
                    if (snapshot.Games.Count == 0)
                    {
                        messages.Add(Serialize(new Dictionary<string, object>
                        {
                            ["type"] = "games", ["payload"] = new object[0]
                        }));
                    }
                    else
                    {
                        for (var index = 0; index < snapshot.Games.Count; index += 100)
                        {
                            messages.Add(Serialize(new Dictionary<string, object>
                            {
                                ["type"] = "games", ["payload"] = snapshot.Games.Skip(index).Take(100).ToArray()
                            }));
                        }
                    }
                    messages.Add(Serialize(new Dictionary<string, object>
                    {
                        ["type"] = "snapshotComplete", ["games"] = snapshot.Games.Count
                    }));
                    if (!target.SendBatch(messages))
                    {
                        log.Warn("Snapshot connection closed before it could be queued");
                        return;
                    }
                    log.Info(string.Format("Snapshot completed: categories={0} games={1}", snapshot.Categories.Count, snapshot.Games.Count));
                }
                catch (Exception ex) { log.Warn("Snapshot failed: " + ex.Message); }
            }
        }

        private void SendStopRequested(string id)
        {
            var payload = Serialize(new Dictionary<string, object>
            {
                ["type"] = "status", ["status"] = new Dictionary<string, object> { ["name"] = "stopRequested", ["id"] = id ?? string.Empty }
            });
            var matches = launchers.Values.Where(x => SameId(x.GameId, id)).ToArray();
            Broadcast(payload, matches.Length == 0 ? null : matches);
        }

        private void SendRunningGames()
        {
            foreach (var game in RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true))
            {
                var payload = dataMapper.BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!game.IsRunning || sunshineGames.ContainsKey(game.Id)) continue;
                    SendCore(payload);
                    var delivered = Broadcast(payload, null);
                    if (delivered == 0) pendingGames.TryAdd(game.Id, 0);
                    sunshineGames.TryAdd(game.Id, 0);
                }
            }
        }

        private void SyncLauncher(PipeConnection launcher)
        {
            var running = RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true);
            var preferred = running.FirstOrDefault(x => SameId(launcher.GameId, x.Id));
            foreach (var game in preferred == null ? running : new[] { preferred })
            {
                var payload = dataMapper.BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!game.IsRunning) continue;
                    var queued = launcher.Send(payload);
                    sunshineGames.TryAdd(game.Id, 0);
                    byte ignored;
                    if (queued) pendingGames.TryRemove(game.Id, out ignored);
                }
            }
        }

        private void FlushPending(PipeConnection launcher)
        {
            foreach (var gameId in pendingGames.Keys.ToArray())
            {
                var game = RunOnUi(() => api.Database.Games.Get(gameId), true);
                var payload = game == null || !game.IsRunning ? null : dataMapper.BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    byte ignored;
                    if (!pendingGames.ContainsKey(gameId)) continue;
                    if (game == null || !game.IsRunning)
                    {
                        pendingGames.TryRemove(gameId, out ignored);
                        continue;
                    }
                    if (launcher.Send(payload)) pendingGames.TryRemove(gameId, out ignored);
                }
            }
        }

        private void SendShutdownHandoff()
        {
            var running = RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true);
            if (running.Length != 0)
            {
                foreach (var game in running) Broadcast(dataMapper.BuildStatus("gameStarted", game), null);
            }
            else
            {
                Broadcast(Serialize(new Dictionary<string, object>
                {
                    ["type"] = "status", ["status"] = new Dictionary<string, object> { ["name"] = "playniteExiting" }
                }), null);
            }
        }

        private void FlushConnections(int timeoutMs)
        {
            var connections = new HashSet<PipeConnection>(launchers.Values);
            var currentCore = GetCore();
            if (currentCore != null) connections.Add(currentCore);
            var elapsed = Stopwatch.StartNew();
            foreach (var connection in connections)
            {
                var remaining = timeoutMs - (int)elapsed.ElapsedMilliseconds;
                if (remaining <= 0 || !connection.Flush(remaining)) break;
            }
        }

        private void SendCore(object message)
        {
            var target = GetCore();
            if (target != null) target.Send(message as string ?? Serialize(message));
        }

        private int Broadcast(string payload, IEnumerable<PipeConnection> targets)
        {
            var count = 0;
            foreach (var target in targets ?? launchers.Values)
            {
                if (target.Send(payload)) count++;
            }
            return count;
        }

        private PipeConnection GetCore() { lock (coreLock) return core; }

        private void ReplaceCore(PipeConnection replacement)
        {
            PipeConnection previous;
            lock (coreLock) { previous = core; core = replacement; }
            if (previous != null && !ReferenceEquals(previous, replacement)) previous.Dispose();
        }

        private Dictionary<string, object> ParseObject(string value)
        {
            return json.Deserialize<Dictionary<string, object>>(value) ?? new Dictionary<string, object>();
        }

        private string Serialize(object value) { lock (json) return json.Serialize(value); }

        private static string GetString(IDictionary<string, object> value, string key)
        {
            object item;
            return value != null && value.TryGetValue(key, out item) && item != null ? Convert.ToString(item) : string.Empty;
        }

        private static int? GetInt(IDictionary<string, object> value, string key)
        {
            object item;
            if (value == null || !value.TryGetValue(key, out item) || item == null) return null;
            try { return Convert.ToInt32(item); } catch { return null; }
        }

        private static IDictionary<string, string> GetEnvironment(IDictionary<string, object> message)
        {
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            object raw;
            var environment = message != null && message.TryGetValue("env", out raw) ? raw as Dictionary<string, object> : null;
            if (environment == null) return result;
            foreach (var pair in environment)
            {
                if (string.IsNullOrWhiteSpace(pair.Key) || pair.Key.IndexOfAny(new[] { '=', '\0' }) >= 0) continue;
                var value = pair.Value == null ? string.Empty : Convert.ToString(pair.Value);
                if (value.IndexOf('\0') < 0) result[pair.Key] = value;
            }
            return result;
        }

        private T RunOnUi<T>(Func<T> action, bool synchronous)
        {
            var dispatcher = api.MainView.UIDispatcher;
            if (dispatcher == null || dispatcher.CheckAccess()) return action();
            return dispatcher.Invoke(action);
        }

        private void RunOnUi(Action action, bool synchronous)
        {
            var dispatcher = api.MainView.UIDispatcher;
            if (dispatcher == null || dispatcher.CheckAccess()) action();
            else if (synchronous) dispatcher.Invoke(action);
            else dispatcher.BeginInvoke(action);
        }

        private static bool SameId(string left, object right)
        {
            if (string.IsNullOrWhiteSpace(left) || right == null) return false;
            return string.Equals(left.Trim().Trim('{', '}'), Convert.ToString(right).Trim().Trim('{', '}'), StringComparison.OrdinalIgnoreCase);
        }

    }
}
