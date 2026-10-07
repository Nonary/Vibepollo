using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;
using System.Text;
using System.Text.Json;
namespace System.Web.Script.Serialization
{
    public class JavaScriptSerializer
    {
        public int MaxJsonLength { get; set; }
        public string Serialize(object value) => JsonSerializer.Serialize(value);
        public T Deserialize<T>(string value) => (T)(object)ConvertObject(JsonDocument.Parse(value).RootElement);
        private static object ConvertObject(JsonElement item)
        {
            if (item.ValueKind == JsonValueKind.Object) return item.EnumerateObject().ToDictionary(p => p.Name, p => ConvertObject(p.Value));
            if (item.ValueKind == JsonValueKind.Number) return item.GetInt32();
            if (item.ValueKind == JsonValueKind.True || item.ValueKind == JsonValueKind.False) return item.GetBoolean();
            if (item.ValueKind == JsonValueKind.Null) return null;
            return item.GetString();
        }
    }
}
namespace System.Windows.Threading
{
    public enum DispatcherPriority { Normal }
    public class DispatcherOperation<T> { public Task<T> Task { get; } public DispatcherOperation(Task<T> task) { Task = task; } }
}
namespace Playnite.SDK
{
    public interface ILogger { }
    public class Logger : ILogger { }
    public class ItemCollectionChangedEventArgs<T> : EventArgs { }
    public class ItemUpdatedEventArgs<T> : EventArgs { }
    public class ItemCollection<T> : List<T>
    {
        public event EventHandler<ItemCollectionChangedEventArgs<T>> ItemCollectionChanged;
        public event EventHandler<ItemUpdatedEventArgs<T>> ItemUpdated;
        public T Get(Guid id) => this.FirstOrDefault(x => ((Models.DatabaseObject)(object)x).Id == id);
        public void Update(T item) { }
    }
    public class FakeDatabase
    {
        public ItemCollection<Models.Game> Games { get; } = new();
        public ItemCollection<Models.Category> Categories { get; } = new();
        public ItemCollection<Models.Emulator> Emulators { get; } = new();
        public string AddFile(string path, Guid id) => path;
        public void RemoveFile(string path) { }
        public string GetFullFilePath(string path) => path;
    }
    // Models queued WPF operations and cancellation; pipe IO below uses real OS pipes.
    public class FakeDispatcher
    {
        public bool QueueActions;
        public bool HoldBackground;
        public readonly int OwnerThread = Thread.CurrentThread.ManagedThreadId;
        public readonly ManualResetEventSlim SyncEntered = new();
        public readonly ManualResetEventSlim SyncRelease = new();
        public System.Collections.Concurrent.ConcurrentQueue<Action> Queue = new();
        public bool CheckAccess() => Thread.CurrentThread.ManagedThreadId == OwnerThread || (!QueueActions && !HoldBackground);
        public T Invoke<T>(Func<T> action) { if (HoldBackground && Thread.CurrentThread.ManagedThreadId != OwnerThread) { SyncEntered.Set(); SyncRelease.Wait(); } return action(); }
        public void Invoke(Action action) => action();
        public void BeginInvoke(Action action) => Queue.Enqueue(action);
        public System.Windows.Threading.DispatcherOperation<T> InvokeAsync<T>(Func<T> action, System.Windows.Threading.DispatcherPriority priority, CancellationToken token)
        {
            var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
            var registration = token.Register(() => completion.TrySetCanceled(token));
            Action execute = () =>
            {
                if (!completion.Task.IsCompleted)
                {
                    try { completion.TrySetResult(action()); } catch (Exception ex) { completion.TrySetException(ex); }
                }
                registration.Dispose();
            };
            if (QueueActions || (HoldBackground && Thread.CurrentThread.ManagedThreadId != OwnerThread))
            {
                Queue.Enqueue(execute); SyncEntered.Set();
            }
            else execute();
            return new System.Windows.Threading.DispatcherOperation<T>(completion.Task);
        }
        public System.Windows.Threading.DispatcherOperation<object> InvokeAsync(Action action, System.Windows.Threading.DispatcherPriority priority, CancellationToken token) => InvokeAsync<object>(() => { action(); return null; }, priority, token);
        public void Drain() { while (Queue.TryDequeue(out var action)) action(); }
    }
    public class FakeMainView { public FakeDispatcher UIDispatcher { get; } = new(); }
    public class FakeAddons { public List<object> Plugins { get; } = new(); }
    public interface IPlayniteAPI
    {
        FakeDatabase Database { get; }
        FakeMainView MainView { get; }
        FakeAddons Addons { get; }
        void StartGame(Guid id);
    }
    public class Api : IPlayniteAPI
    {
        public FakeDatabase Database { get; } = new();
        public FakeMainView MainView { get; } = new();
        public FakeAddons Addons { get; } = new();
        public List<Guid> Started = new();
        public void StartGame(Guid id) => Started.Add(id);
    }
}
namespace Playnite.SDK.Models
{
    public class DatabaseObject { public Guid Id { get; set; } public string Name { get; set; } }
    public class Category : DatabaseObject { }
    public class Emulator : DatabaseObject { public string InstallDir { get; set; } }
    public class GameAction
    {
        public bool IsPlayAction; public string Path, Arguments, WorkingDir; public Guid EmulatorId; public object Type;
    }
    public class Game : DatabaseObject
    {
        public string InstallDirectory, CoverImage, Icon;
        public List<GameAction> GameActions;
        public List<Guid> CategoryIds;
        public Guid PluginId;
        public long Playtime;
        public DateTime? LastActivity;
        public bool IsInstalled, IsRunning;
    }
}
namespace Playnite.SDK.Plugins { public class LibraryPlugin { public Guid Id; public string Name; } }
namespace SunshinePlaynite
{
    internal sealed class ConnectorLog : IDisposable
    {
        public ConnectorLog(Playnite.SDK.ILogger logger, bool enabled) { }
        public void Info(string message) => Console.WriteLine(message);
        public void Debug(string message) { }
        public void Warn(string message) => Console.WriteLine(message);
        public void SetDebugEnabled(bool enabled) { }
        public void Dispose() { }
    }
    internal static class ConnectorPipeTransport
    {
        public static readonly ManualResetEventSlim Validating = new();
        public static readonly ManualResetEventSlim ContinueValidation = new();
        public static bool PauseValidation;
        public static string TestSuffix = Guid.NewGuid().ToString("N");
        public static object CreatePipeSecurity() => new();
        public static NamedPipeServerStream CreateServer(string name, object security) => new(name + TestSuffix, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 65536, 65536);
        public static void WaitForConnection(NamedPipeServerStream pipe, CancellationToken token, int timeoutMs = Timeout.Infinite) => pipe.WaitForConnectionAsync(token).GetAwaiter().GetResult();
        public static void WriteHandshake(Stream stream, string name)
        {
            var bytes = Encoding.Unicode.GetBytes((name + '\0').PadRight(40, '\0'));
            stream.Write(bytes, 0, bytes.Length); stream.Flush();
        }
        public static bool WaitForAck(Stream stream) => stream.ReadByte() == 2;
        public static string ReadLine(StreamReader reader, CancellationToken token, int timeoutMs) => reader.ReadLine();
        public static string ValidateClient(NamedPipeServerStream stream, IDictionary<string, object> hello)
        {
            if (PauseValidation) { Validating.Set(); ContinueValidation.Wait(); }
            return (string)hello["role"];
        }
    }
}
