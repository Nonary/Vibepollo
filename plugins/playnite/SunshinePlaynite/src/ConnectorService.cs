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
using System.Windows.Threading;
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
        private readonly object connectionLock = new object();
        private readonly object coreLock = new object();
        private readonly object gameStateLock = new object();
        // RunTemporary holds the scope lock across StartGame, which can call
        // GameStarted synchronously. Never hold gameStateLock while setting or
        // clearing a scope, or that callback can deadlock against a pipe worker.
        private readonly object environmentLock = new object();
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
                var token = nextCancellation.Token;
                try
                {
                    api.Database.Games.ItemCollectionChanged += GamesChanged;
                    api.Database.Games.ItemUpdated += GamesUpdated;
                    snapshotTimer = new Timer(_ => SendSnapshot(token), null, Timeout.Infinite, Timeout.Infinite);
                    lock (connectionLock)
                    {
                        cancellation = nextCancellation;
                        Volatile.Write(ref started, 1);
                    }
                    serverTask = Task.Factory.StartNew(() => ServerLoop(token), CancellationToken.None,
                        TaskCreationOptions.LongRunning, TaskScheduler.Default);
                    log.Info("Compiled Playnite plugin started");
                }
                catch
                {
                    try { api.Database.Games.ItemCollectionChanged -= GamesChanged; } catch { }
                    try { api.Database.Games.ItemUpdated -= GamesUpdated; } catch { }
                    try { if (snapshotTimer != null) snapshotTimer.Dispose(); } catch { }
                    snapshotTimer = null;
                    serverTask = null;
                    lock (connectionLock)
                    {
                        Volatile.Write(ref started, 0);
                        cancellation = null;
                    }
                    nextCancellation.Cancel();
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
                // Close admission before flushing, using the same lock as
                // publication so no connection can appear after the stop sweep.
                lock (connectionLock) Volatile.Write(ref started, 0);
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

                    CancellationTokenSource stopCancellation;
                    NamedPipeServerStream stopPipe;
                    lock (connectionLock)
                    {
                        stopCancellation = cancellation;
                        cancellation = null;
                        stopPipe = pendingPipe;
                        pendingPipe = null;
                    }
                    try { if (stopCancellation != null) stopCancellation.Cancel(); } catch { }
                    try { if (stopPipe != null) stopPipe.Dispose(); } catch { }
                    try { ReplaceCore(null); } catch { }
                    lock (environmentLock)
                    {
                        foreach (var item in launchers.ToArray())
                        {
                            PipeConnection ignored;
                            if (launchers.TryRemove(item.Key, out ignored))
                            {
                                try { ignored.Dispose(); } catch { }
                            }
                            try { environmentScopes.Clear(item.Key); } catch { }
                        }
                    }
                    lock (gameStateLock)
                    {
                        pendingGames.Clear();
                        sunshineGames.Clear();
                    }

                    var task = serverTask;
                    serverTask = null;
                    try { if (task != null) task.Wait(1000); } catch { }
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
            if (game == null || Volatile.Read(ref started) == 0) return;
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
            if (game == null || Volatile.Read(ref started) == 0) return;
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
            try { if (timer != null) timer.Change(3000, Timeout.Infinite); }
            catch (ObjectDisposedException) { } // An already delivered database event raced with Stop.
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
                PipeConnection unownedConnection = null;
                try
                {
                    control = ConnectorPipeTransport.CreateServer(ControlPipeName, security);
                    if (!SetPendingPipe(control, token)) break;
                    ConnectorPipeTransport.WaitForConnection(control, token);
                    var pipeName = Guid.NewGuid().ToString("B").ToUpperInvariant();
                    data = ConnectorPipeTransport.CreateServer(pipeName, security);
                    ConnectorPipeTransport.WriteHandshake(control, pipeName);
                    if (!ConnectorPipeTransport.WaitForAck(control)) throw new IOException("Handshake ACK missing");
                    ClearPendingPipe(control);
                    control.Dispose();
                    control = null;
                    if (!SetPendingPipe(data, token)) break;
                    ConnectorPipeTransport.WaitForConnection(data, token, DataConnectionTimeoutMs);

                    var reader = new StreamReader(data, new UTF8Encoding(false), false, 8192, true);
                    var writer = new StreamWriter(data, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
                    var helloLine = ConnectorPipeTransport.ReadLine(reader, token, HelloTimeoutMs);
                    if (helloLine == null) throw new IOException("No hello received");
                    var hello = ParseObject(helloLine);
                    var role = ConnectorPipeTransport.ValidateClient(data, hello);
                    unownedConnection = new PipeConnection(pipeName, data, reader, writer, log);
                    unownedConnection.Pid = GetInt(hello, "pid");
                    unownedConnection.GameId = GetString(hello, "gameId");
                    PipeConnection previous = null;
                    lock (connectionLock)
                    {
                        if (!IsCurrentRun(token)) break;
                        if (string.Equals(role, "sunshine", StringComparison.OrdinalIgnoreCase))
                        {
                            lock (coreLock) { previous = core; core = unownedConnection; }
                            StartCore(unownedConnection, token);
                        }
                        else
                        {
                            launchers[pipeName] = unownedConnection;
                            StartLauncher(unownedConnection, token);
                        }
                        if (ReferenceEquals(pendingPipe, data)) pendingPipe = null;
                        // The registered worker owns cleanup even if its run
                        // was cancelled before the task began executing.
                        data = null;
                        unownedConnection = null;
                    }
                    if (previous != null) previous.Dispose();
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
                    ClearPendingPipe(control);
                    ClearPendingPipe(data);
                    if (unownedConnection != null) unownedConnection.Dispose();
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
                    token.ThrowIfCancellationRequested();
                    SendSnapshot(token);
                    SendRunningGames(token);
                    ReadCore(connection, token);
                }
                catch (Exception ex) { if (!token.IsCancellationRequested) log.Warn("Core reader failed: " + ex.Message); }
                finally
                {
                    lock (coreLock)
                    {
                        if (ReferenceEquals(core, connection)) core = null;
                    }
                    connection.Dispose();
                }
            }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        private void ReadCore(PipeConnection connection, CancellationToken token)
        {
            while (!token.IsCancellationRequested && ReferenceEquals(GetCore(), connection))
            {
                var line = connection.Reader.ReadLine();
                if (line == null) break;
                try { if (IsCurrentRun(token)) HandleCoreCommand(ParseObject(line), token); }
                catch (Exception ex) { log.Warn("Failed to handle Sunshine command: " + ex.Message); }
            }
        }

        private void HandleCoreCommand(Dictionary<string, object> message, CancellationToken token)
        {
            if (GetString(message, "type") != "command") return;
            var command = GetString(message, "command");
            var id = GetString(message, "id");
            if (command == "launch" && Guid.TryParse(id, out var gameId))
            {
                lock (gameStateLock)
                {
                    if (!IsCurrentRun(token)) return;
                    sunshineGames.TryAdd(gameId, 0);
                }
                var environment = GetEnvironment(message);
                RunOnUi(() => environmentScopes.RunTemporary(environment, () => api.StartGame(gameId)), false, token);
            }
            else if (command == "stop")
            {
                SendStopRequested(id);
            }
            else if (command == "snapshot")
            {
                SendSnapshot(token);
            }
            else if (command == "set-cover")
            {
                var success = false;
                var error = string.Empty;
                try
                {
                    var path = GetString(message, "path");
                    if (!Guid.TryParse(id, out gameId) || !File.Exists(path)) throw new IOException("Invalid game ID or cover path");
                    success = RunOnUi(() => dataMapper.SetCover(gameId, path), true, token);
                    if (!success) throw new InvalidOperationException("Playnite rejected the cover metadata update");
                }
                catch (Exception ex) { error = ex.Message; }
                SendCore(new Dictionary<string, object>
                {
                    ["type"] = "commandResult", ["command"] = "set-cover",
                    ["requestId"] = GetString(message, "requestId"), ["success"] = success, ["error"] = error
                }, token);
            }
        }

        private void StartLauncher(PipeConnection connection, CancellationToken token)
        {
            log.Info(string.Format("Launcher connection accepted id={0} pid={1}", connection.Id, connection.Pid));
            Task.Factory.StartNew(() =>
            {
                try
                {
                    token.ThrowIfCancellationRequested();
                    SyncLauncher(connection, token);
                    FlushPending(connection, token);
                    while (!token.IsCancellationRequested)
                    {
                        var line = connection.Reader.ReadLine();
                        if (line == null) break;
                        var message = ParseObject(line);
                        if (!IsCurrentRun(token)) break;
                        if (GetString(message, "type") != "command") continue;
                        var command = GetString(message, "command");
                        if (command == "launch" && Guid.TryParse(GetString(message, "id"), out var gameId))
                        {
                            lock (gameStateLock)
                            {
                                if (!IsCurrentRun(token)) break;
                                sunshineGames.TryAdd(gameId, 0);
                            }
                            lock (environmentLock)
                            {
                                if (!IsCurrentRun(token)) break;
                                environmentScopes.Set(connection.Id, GetEnvironment(message));
                            }
                            RunOnUi(() => api.StartGame(gameId), false, token);
                        }
                        else if (command == "set-environment")
                        {
                            lock (environmentLock)
                            {
                                if (!IsCurrentRun(token)) break;
                                environmentScopes.Set(connection.Id, GetEnvironment(message));
                            }
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
            }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        private void SendSnapshot(CancellationToken token)
        {
            lock (snapshotLock)
            {
                if (!IsCurrentRun(token)) return;
                var target = GetCore();
                if (target == null) return;
                try
                {
                    var snapshot = RunOnUi(() => dataMapper.BuildSnapshot(), true, token);
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

        private void SendRunningGames(CancellationToken token)
        {
            foreach (var game in RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true, token))
            {
                var payload = dataMapper.BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!IsCurrentRun(token)) return;
                    if (!game.IsRunning || sunshineGames.ContainsKey(game.Id)) continue;
                    SendCore(payload, token);
                    var delivered = Broadcast(payload, null);
                    if (delivered == 0) pendingGames.TryAdd(game.Id, 0);
                    sunshineGames.TryAdd(game.Id, 0);
                }
            }
        }

        private void SyncLauncher(PipeConnection launcher, CancellationToken token)
        {
            var running = RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true, token);
            var preferred = running.FirstOrDefault(x => SameId(launcher.GameId, x.Id));
            foreach (var game in preferred == null ? running : new[] { preferred })
            {
                var payload = dataMapper.BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!IsCurrentRun(token)) return;
                    if (!game.IsRunning) continue;
                    var queued = launcher.Send(payload);
                    sunshineGames.TryAdd(game.Id, 0);
                    byte ignored;
                    if (queued) pendingGames.TryRemove(game.Id, out ignored);
                }
            }
        }

        private void FlushPending(PipeConnection launcher, CancellationToken token)
        {
            foreach (var gameId in pendingGames.Keys.ToArray())
            {
                var game = RunOnUi(() => api.Database.Games.Get(gameId), true, token);
                var payload = game == null || !game.IsRunning ? null : dataMapper.BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!IsCurrentRun(token)) return;
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

        private void SendCore(object message, CancellationToken token = default(CancellationToken))
        {
            if (token.CanBeCanceled && !IsCurrentRun(token)) return;
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

        private bool IsCurrentRun(CancellationToken token)
        {
            lock (connectionLock)
            {
                return Volatile.Read(ref started) != 0 && cancellation != null &&
                    !token.IsCancellationRequested && cancellation.Token == token;
            }
        }

        private bool SetPendingPipe(NamedPipeServerStream pipe, CancellationToken token)
        {
            lock (connectionLock)
            {
                if (!IsCurrentRun(token)) return false;
                pendingPipe = pipe;
                return true;
            }
        }

        private void ClearPendingPipe(NamedPipeServerStream pipe)
        {
            lock (connectionLock)
            {
                if (ReferenceEquals(pendingPipe, pipe)) pendingPipe = null;
            }
        }

        private void ReplaceCore(PipeConnection replacement)
        {
            PipeConnection previous;
            lock (coreLock) { previous = core; core = replacement; }
            if (previous != null && !ReferenceEquals(previous, replacement)) previous.Dispose();
        }

        private Dictionary<string, object> ParseObject(string value)
        {
            lock (json) return json.Deserialize<Dictionary<string, object>>(value) ?? new Dictionary<string, object>();
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

        private T RunOnUi<T>(Func<T> action, bool synchronous, CancellationToken token = default(CancellationToken))
        {
            token.ThrowIfCancellationRequested();
            if (token.CanBeCanceled && !IsCurrentRun(token)) throw new OperationCanceledException(token);
            var dispatcher = api.MainView.UIDispatcher;
            if (dispatcher == null || dispatcher.CheckAccess()) return action();
            // A shutdown on the UI thread must be able to cancel a worker's
            // queued query instead of waiting for that same UI thread.
            return dispatcher.InvokeAsync(action, DispatcherPriority.Normal, token).Task.GetAwaiter().GetResult();
        }

        private void RunOnUi(Action action, bool synchronous, CancellationToken token = default(CancellationToken))
        {
            var dispatcher = api.MainView.UIDispatcher;
            Action guarded = () =>
            {
                if (token.CanBeCanceled && !IsCurrentRun(token)) return;
                action();
            };
            if (dispatcher == null || dispatcher.CheckAccess()) guarded();
            else if (synchronous) dispatcher.InvokeAsync(guarded, DispatcherPriority.Normal, token).Task.GetAwaiter().GetResult();
            else dispatcher.InvokeAsync(guarded, DispatcherPriority.Normal, token);
        }

        private static bool SameId(string left, object right)
        {
            if (string.IsNullOrWhiteSpace(left) || right == null) return false;
            return string.Equals(left.Trim().Trim('{', '}'), Convert.ToString(right).Trim().Trim('{', '}'), StringComparison.OrdinalIgnoreCase);
        }

    }
}
