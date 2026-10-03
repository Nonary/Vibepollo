using System;
using System.Linq;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;
using Playnite.SDK;
using Playnite.SDK.Models;
using SunshinePlaynite;
class Program
{
    static object Field(object target, string name) => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);
    static void Call(object target, string name, params object[] args) => target.GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic).Invoke(target, args);
    static void Assert(bool condition, string text) { if (!condition) throw new Exception(text); Console.WriteLine("PASS: " + text); }
    static void Main()
    {
        AcceptDuringStop("launcher", false);
        AcceptDuringStop("sunshine", true);
        CancelledReaderStillCleansUp();
        LaunchQueuedBeforeStop();
        StalledWriter();
        StopWithPendingUi();
        TemporaryEnvironmentDoesNotBlockGameStatus();
        RoundTrip();
    }
    static void AcceptDuringStop(string role, bool restart)
    {
        Console.WriteLine("CASE: stop while an incoming client completes validation");
        var api = new Api(); var service = new ConnectorService(api, new Logger(), false);
        ConnectorPipeTransport.PauseValidation = true;
        ConnectorPipeTransport.Validating.Reset();
        ConnectorPipeTransport.ContinueValidation.Reset();
        ConnectorPipeTransport.TestSuffix = Guid.NewGuid().ToString("N");
        service.Start();
        using var control = new NamedPipeClientStream(".", "Sunshine.PlayniteExtension" + ConnectorPipeTransport.TestSuffix, PipeDirection.InOut, PipeOptions.Asynchronous);
        control.Connect(5000);
        var handshake = new byte[80]; control.ReadExactly(handshake);
        string name = Encoding.Unicode.GetString(handshake).TrimEnd('\0');
        control.WriteByte(2); control.Flush();
        using var client = new NamedPipeClientStream(".", name + ConnectorPipeTransport.TestSuffix, PipeDirection.InOut, PipeOptions.Asynchronous);
        client.Connect(5000);
        using var writer = new StreamWriter(client, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
        writer.WriteLine("{\"type\":\"hello\",\"role\":\"" + role + "\",\"pid\":123}");
        Assert(ConnectorPipeTransport.Validating.Wait(5000), "Incoming hello reached validation");
        var oldServer = (Task)Field(service, "serverTask");
        var elapsed = Stopwatch.StartNew(); service.Stop();
        Console.WriteLine("Stop returned after " + elapsed.ElapsedMilliseconds + "ms");
        Assert(!oldServer.IsCompleted, "Stop returned while accept worker was still running");
        if (restart) service.Start();
        ConnectorPipeTransport.ContinueValidation.Set();
        Assert(oldServer.Wait(5000), "Accept worker subsequently exited");
        var connections = (ConcurrentDictionary<string, PipeConnection>)Field(service, "launchers");
        Assert(connections.Count == 0, "Client completing validation after Stop is rejected");
        Assert(Field(service, "core") == null, "No core survives shutdown");
        service.Stop(); service.Dispose();

    }
    static void CancelledReaderStillCleansUp()
    {
        Console.WriteLine("CASE: cancellation before a reader task starts still runs cleanup");
        var name = "pr513-cancelled-" + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
        using var client = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);
        var connect = Task.Run(() => client.Connect(5000));
        server.WaitForConnection();
        connect.Wait();
        var connection = new PipeConnection(name, server,
            new StreamReader(server, new UTF8Encoding(false), false, 8192, true),
            new StreamWriter(server, new UTF8Encoding(false), 8192, true),
            new ConnectorLog(new Logger(), false));
        using var service = new ConnectorService(new Api(), new Logger(), false);
        ((ConcurrentDictionary<string, PipeConnection>)Field(service, "launchers"))[name] = connection;
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        Call(service, "StartLauncher", connection, cancellation.Token);
        Assert(SpinWait.SpinUntil(() => (int)Field(connection, "disposed") != 0, 5000), "Cancelled reader disposes its connection");
        Assert(((Task)Field(connection, "writerTask")).Wait(5000), "Cancelled reader leaves no writer running");
    }
    static void LaunchQueuedBeforeStop()
    {
        Console.WriteLine("CASE: launch queued to UI just before connector disabled");
        var api = new Api(); var service = new ConnectorService(api, new Logger(), false);
        ConnectorPipeTransport.PauseValidation = false;
        ConnectorPipeTransport.TestSuffix = Guid.NewGuid().ToString("N");
        service.Start();
        api.MainView.UIDispatcher.QueueActions = true;
        var gameId = Guid.NewGuid();
        Task.Run(() => Call(service, "HandleCoreCommand", new Dictionary<string, object> { { "type", "command" }, { "command", "launch" }, { "id", gameId.ToString() } }, ((CancellationTokenSource)Field(service, "cancellation")).Token)).Wait();
        Assert(api.MainView.UIDispatcher.Queue.Count == 1, "Launch queued for UI dispatch");
        service.Stop();
        service.Start();
        api.MainView.UIDispatcher.Drain();
        Assert(!api.Started.Contains(gameId), "Old queued launch is cancelled across Stop and Start");
        service.Dispose();
    }

    static void StalledWriter()
    {
        Console.WriteLine("CASE: pipe peer stops reading a large outgoing message");
        var name = "pr513-stall-" + Guid.NewGuid().ToString("N");
        using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 1024, 1024);
        using var client = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);
        var connect = Task.Run(() => client.Connect(5000)); server.WaitForConnection(); connect.Wait();
        var reader = new StreamReader(server, new UTF8Encoding(false), false, 8192, true);
        var writer = new StreamWriter(server, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
        var connection = new PipeConnection(name, server, reader, writer, new ConnectorLog(new Logger(), false));
        connection.Send(new string('a', 4 * 1024 * 1024));
        Assert(!connection.Flush(150), "Flush times out when client does not read");
        var elapsed = Stopwatch.StartNew(); connection.Dispose();
        Console.WriteLine("Blocked-write Dispose returned after " + elapsed.ElapsedMilliseconds + "ms");
        Assert(elapsed.ElapsedMilliseconds < 2000, "Disposal of a stalled writer returns within two seconds in the Linux harness");
    }
    static void StopWithPendingUi()
    {
        Console.WriteLine("CASE: close with stalled snapshot writes while server waits for a UI query");
        var api = new Api(); var service = new ConnectorService(api, new Logger(), false);
        var ui = api.MainView.UIDispatcher; ui.HoldBackground = true;
        ConnectorPipeTransport.PauseValidation = false;
        ConnectorPipeTransport.TestSuffix = Guid.NewGuid().ToString("N");
        service.Start();
        using var control = new NamedPipeClientStream(".", "Sunshine.PlayniteExtension" + ConnectorPipeTransport.TestSuffix, PipeDirection.InOut, PipeOptions.Asynchronous);
        control.Connect(5000); var handshake = new byte[80]; control.ReadExactly(handshake);
        var name = Encoding.Unicode.GetString(handshake).TrimEnd('\0'); control.WriteByte(2); control.Flush();
        using var client = new NamedPipeClientStream(".", name + ConnectorPipeTransport.TestSuffix, PipeDirection.InOut, PipeOptions.Asynchronous);
        client.Connect(5000);
        using var writer = new StreamWriter(client, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
        writer.WriteLine("{\"type\":\"hello\",\"role\":\"launcher\",\"pid\":123}");
        Assert(ui.SyncEntered.Wait(5000), "Launcher worker has a queued UI query");
        var coreName = "pr513-core-" + Guid.NewGuid().ToString("N");
        using var coreServer = new NamedPipeServerStream(coreName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 1024, 1024);
        using var coreClient = new NamedPipeClientStream(".", coreName, PipeDirection.InOut, PipeOptions.Asynchronous);
        var connect = Task.Run(() => coreClient.Connect(5000)); coreServer.WaitForConnection(); connect.Wait();
        var core = new PipeConnection(coreName, coreServer, new StreamReader(coreServer, new UTF8Encoding(false), false, 8192, true), new StreamWriter(coreServer, new UTF8Encoding(false), 8192, true) { AutoFlush = true }, new ConnectorLog(new Logger(), false));
        Call(service, "ReplaceCore", core);
        core.Send(new string('s', 4 * 1024 * 1024));
        Thread.Sleep(80);
        var launcher = ((ConcurrentDictionary<string, PipeConnection>)Field(service, "launchers")).Values.Single();
        var oldServer = (Task)Field(service, "serverTask");
        var elapsed = Stopwatch.StartNew(); service.Stop();
        Console.WriteLine("Stop with pending UI query returned after " + elapsed.ElapsedMilliseconds + "ms");
        Assert(elapsed.ElapsedMilliseconds < 1200, "Stop cancels pending UI queries without a server timeout");
        Assert(oldServer.Wait(5000), "Accept worker exits during shutdown");
        Assert(((Task)Field(launcher, "writerTask")).Wait(5000), "Launcher writer exits despite a pending UI query");
        Assert(((ConcurrentDictionary<string, PipeConnection>)Field(service, "launchers")).IsEmpty, "Shutdown removes launcher");
        ui.Drain(); service.Dispose();
    }
    static bool CanEnter(object gate)
    {
        if (!Monitor.TryEnter(gate)) return false;
        Monitor.Exit(gate);
        return true;
    }
    static void TemporaryEnvironmentDoesNotBlockGameStatus()
    {
        Console.WriteLine("CASE: temporary launch environment and concurrent launcher environment change");
        ConnectorPipeTransport.TestSuffix = Guid.NewGuid().ToString("N");
        var api = new Api();
        using var service = new ConnectorService(api, new Logger(), false);
        service.Start();
        using var launcher = Connect("launcher");
        Assert(SpinWait.SpinUntil(() => ((ConcurrentDictionary<string, PipeConnection>)Field(service, "launchers")).Count == 1, 5000), "Launcher accepted for environment test");
        var scopes = (EnvironmentScopes)Field(service, "environmentScopes");
        var key = "VIBEPOLLO_TEST_" + Guid.NewGuid().ToString("N");
        using var holdingScope = new ManualResetEventSlim();
        using var runCallback = new ManualResetEventSlim();
        var callback = Task.Run(() => scopes.RunTemporary(new Dictionary<string, string>(), () =>
        {
            holdingScope.Set();
            runCallback.Wait();
            service.GameStarted(new Game { Id = Guid.NewGuid(), Name = "Callback game" });
        }));
        Assert(holdingScope.Wait(5000), "Launch callback holds temporary environment scope");
        using var writer = new StreamWriter(launcher, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
        writer.WriteLine(System.Text.Json.JsonSerializer.Serialize(new
        {
            type = "command", command = "set-environment", env = new Dictionary<string, string> { [key] = "launcher" }
        }));
        var environmentGate = Field(service, "environmentLock");
        var gameGate = Field(service, "gameStateLock");
        try
        {
            Assert(SpinWait.SpinUntil(() => !CanEnter(environmentGate) || !CanEnter(gameGate), 5000), "Launcher worker waits for temporary scope");
            Assert(CanEnter(gameGate), "Environment update leaves game state available to callbacks");
        }
        finally { runCallback.Set(); }
        Assert(callback.Wait(5000), "GameStarted callback completes while launcher environment update waits");
        Assert(SpinWait.SpinUntil(() => Environment.GetEnvironmentVariable(key) == "launcher", 5000), "Environment update completes after callback");
        service.Stop();
        Assert(Environment.GetEnvironmentVariable(key) == null, "Shutdown restores launcher environment");
    }
    static void RoundTrip()
    {
        Console.WriteLine("CASE: snapshots and shutdown handoff over real pipes");
        ConnectorPipeTransport.TestSuffix = Guid.NewGuid().ToString("N");
        var api = new Api(); using var service = new ConnectorService(api, new Logger(), false);
        service.Start();
        using var core = Connect("sunshine");
        using var reader = new StreamReader(core, new UTF8Encoding(false), false, 8192, true);
        var types = new List<string>();
        for (int i = 0; i < 5; i++) types.Add(System.Text.Json.JsonDocument.Parse(reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(5)).GetAwaiter().GetResult()).RootElement.GetProperty("type").GetString());
        Assert(types.SequenceEqual(new[] { "snapshotStart", "plugins", "categories", "games", "snapshotComplete" }), "Empty library snapshot keeps protocol ordering");
        var game = new Game { Id = Guid.NewGuid(), Name = "Test game", IsRunning = true };
        using var launcher = Connect("launcher", game.Id.ToString());
        using var launcherReader = new StreamReader(launcher, new UTF8Encoding(false), false, 8192, true);
        Assert(SpinWait.SpinUntil(() => ((ConcurrentDictionary<string, PipeConnection>)Field(service, "launchers")).Count == 1, 5000), "Launcher accepted");
        service.GameStarted(game);
        Assert(ReadLine(launcherReader).Contains("gameStarted"), "Game start reaches launcher");
        Assert(ReadLine(reader).Contains("gameStarted"), "Game start reaches core");
        game.IsRunning = false;
        service.GameStopped(game);
        Assert(ReadLine(launcherReader).Contains("gameStopped"), "Game stop reaches launcher");
        Assert(ReadLine(reader).Contains("gameStopped"), "Game stop reaches core");
        service.Stop();
        var status = launcherReader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(5)).GetAwaiter().GetResult();
        Assert(status.Contains("playniteExiting"), "Shutdown handoff reaches launcher before disposal");
    }
    static string ReadLine(StreamReader reader) => reader.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(5)).GetAwaiter().GetResult();

    static NamedPipeClientStream Connect(string role, string gameId = "")
    {
        using var control = new NamedPipeClientStream(".", "Sunshine.PlayniteExtension" + ConnectorPipeTransport.TestSuffix, PipeDirection.InOut, PipeOptions.Asynchronous);
        control.Connect(5000); var bytes = new byte[80]; control.ReadExactly(bytes);
        var name = Encoding.Unicode.GetString(bytes).TrimEnd('\0'); control.WriteByte(2); control.Flush();
        var client = new NamedPipeClientStream(".", name + ConnectorPipeTransport.TestSuffix, PipeDirection.InOut, PipeOptions.Asynchronous);
        client.Connect(5000);
        using var writer = new StreamWriter(client, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
        writer.WriteLine(System.Text.Json.JsonSerializer.Serialize(new { type = "hello", role, pid = 123, gameId }));
        return client;
    }
}
