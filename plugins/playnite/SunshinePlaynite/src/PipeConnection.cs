using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;

namespace SunshinePlaynite
{
    internal sealed class PipeConnection : IDisposable
    {
        private readonly BlockingCollection<OutboundMessage> outbox = new BlockingCollection<OutboundMessage>();
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly AutoResetEvent writeProgress = new AutoResetEvent(false);
        private readonly object sendLock = new object();
        private readonly ConnectorLog log;
        private readonly Task writerTask;
        private long nextSequence;
        private long completedSequence;
        private int disposed;

        public PipeConnection(string id, NamedPipeServerStream stream, StreamReader reader, StreamWriter writer, ConnectorLog log)
        {
            Id = id;
            Stream = stream;
            Reader = reader;
            Writer = writer;
            this.log = log;
            writerTask = Task.Factory.StartNew(WriteLoop, cancellation.Token, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        public string Id { get; private set; }
        public int? Pid { get; set; }
        public string GameId { get; set; }
        public NamedPipeServerStream Stream { get; private set; }
        public StreamReader Reader { get; private set; }
        public StreamWriter Writer { get; private set; }

        public bool Send(string payload)
        {
            return SendBatch(new[] { payload });
        }

        public bool SendBatch(IEnumerable<string> payloads)
        {
            if (payloads == null) return false;
            lock (sendLock)
            {
                if (Volatile.Read(ref disposed) != 0) return false;
                try
                {
                    foreach (var payload in payloads)
                    {
                        var sequence = nextSequence + 1;
                        outbox.Add(new OutboundMessage(sequence, payload));
                        Volatile.Write(ref nextSequence, sequence);
                    }
                    return true;
                }
                catch { return false; }
            }
        }

        public bool Flush(int timeoutMs)
        {
            var target = Volatile.Read(ref nextSequence);
            if (Volatile.Read(ref completedSequence) >= target) return true;
            var elapsed = Stopwatch.StartNew();
            try
            {
                while (Volatile.Read(ref disposed) == 0 && Volatile.Read(ref completedSequence) < target)
                {
                    var remaining = timeoutMs - (int)elapsed.ElapsedMilliseconds;
                    if (remaining <= 0 || !writeProgress.WaitOne(remaining)) return false;
                }
                return Volatile.Read(ref completedSequence) >= target;
            }
            catch (ObjectDisposedException) { return false; }
        }

        private void WriteLoop()
        {
            try
            {
                while (!cancellation.IsCancellationRequested)
                {
                    OutboundMessage message;
                    if (!outbox.TryTake(out message, 500)) continue;
                    Writer.WriteLine(message.Payload);
                    Writer.Flush();
                    Volatile.Write(ref completedSequence, message.Sequence);
                    writeProgress.Set();
                }
            }
            catch (Exception ex)
            {
                if (!cancellation.IsCancellationRequested) log.Debug("Pipe writer stopped: " + ex.Message);
            }
            finally
            {
                try { writeProgress.Set(); } catch { }
            }
        }

        public void Dispose()
        {
            lock (sendLock)
            {
                if (Interlocked.Exchange(ref disposed, 1) != 0) return;
                try { outbox.CompleteAdding(); } catch { }
            }
            try { writeProgress.Set(); } catch { }
            try { cancellation.Cancel(); } catch { }
            try { Stream.Dispose(); } catch { }
            try { writerTask.Wait(500); } catch { }
            try { Reader.Dispose(); } catch { }
            try { Writer.Dispose(); } catch { }
            try { outbox.Dispose(); } catch { }
            try { writeProgress.Dispose(); } catch { }
            try { cancellation.Dispose(); } catch { }
        }

        private sealed class OutboundMessage
        {
            public OutboundMessage(long sequence, string payload)
            {
                Sequence = sequence;
                Payload = payload;
            }

            public long Sequence { get; private set; }
            public string Payload { get; private set; }
        }
    }
}
