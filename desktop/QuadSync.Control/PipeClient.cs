using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace QuadSync.Control;
public sealed class PipeClient : IDisposable
{
    private NamedPipeClientStream? pipe;
    private StreamReader? reader;
    private StreamWriter? writer;
    private readonly SemaphoreSlim gate = new(1, 1);
    public bool Connected => pipe?.IsConnected == true;
    public async Task ConnectAsync(int milliseconds)
    {
        Dispose();
        pipe = new NamedPipeClientStream(".", "QuadSyncAudio.v1", PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(milliseconds);
        reader = new StreamReader(pipe, new UTF8Encoding(false), false, 4096, true);
        writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, true) { AutoFlush = true, NewLine = "\n" };
    }
    public async Task<JsonElement> SendAsync(object request)
    {
        await gate.WaitAsync();
        try
        {
            if (!Connected || writer is null || reader is null) throw new IOException("引擎连接已断开");
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(8));
            await writer.WriteLineAsync(JsonSerializer.Serialize(request).AsMemory(), timeout.Token);
            var line = await reader.ReadLineAsync(timeout.Token) ?? throw new IOException("引擎连接已关闭");
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.GetProperty("ok").GetBoolean()) throw new EngineException(root.GetProperty("error").GetString() ?? "引擎错误");
            return root.GetProperty("data").Clone();
        }
        catch (EngineException) { throw; }
        catch { Dispose(); throw; }
        finally { gate.Release(); }
    }
    public void Dispose() { pipe?.Dispose(); pipe = null; reader = null; writer = null; }
}
public sealed class EngineException(string message) : Exception(message);
