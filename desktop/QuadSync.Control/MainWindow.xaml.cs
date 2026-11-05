using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;

namespace QuadSync.Control;
public partial class MainWindow : Window
{
    private readonly ConsoleModel model = new();
    private bool closing;
    public MainWindow() { InitializeComponent(); DataContext = model; }
    private async void OnLoaded(object sender, RoutedEventArgs e) => await model.InitializeAsync();
    private async void RefreshClick(object sender, RoutedEventArgs e) => await model.InitializeAsync();
    private async void TransportClick(object sender, RoutedEventArgs e) => await model.ToggleAsync();
    private void BrowseClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "WAV 音频|*.wav", Title = "选择 48 kHz 双声道或四声道 WAV" };
        if (dialog.ShowDialog(this) == true) { model.WavPath = dialog.FileName; model.SourceIndex = 1; }
    }
    private async void OnClosing(object? sender, CancelEventArgs e)
    {
        if (closing) return;
        e.Cancel = true; closing = true;
        await model.CloseAsync(); Close();
    }
}

public sealed class ConsoleModel : Observable
{
    public ObservableCollection<Device> Devices { get; } = [];
    public Channel[] Channels { get; } = [
        new() { Name = "前置左", Subtitle = "FRONT LEFT · A", Number = "01" },
        new() { Name = "前置右", Subtitle = "FRONT RIGHT · A", Number = "02" },
        new() { Name = "后置左", Subtitle = "REAR LEFT · B", Number = "03" },
        new() { Name = "后置右", Subtitle = "REAR RIGHT · B", Number = "04" } ];
    private readonly PipeClient client = new();
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(50) };
    private readonly string settingsPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "QuadSyncAudio", "settings.json");
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };
    private Process? ownedEngine;
    private bool busy, running, polling, pendingSettings = true, closing, connected;
    private DateTime editedAt;
    private Device? deviceA, deviceB;
    private string savedA = "", savedB = "", path = "", status = "连接引擎后，选择两台支持 Float32 的输出设备。";
    private string lagLabel = "0.0 frames", syncLabel = "等待音频时钟", ratioLabel = "补偿  +0.0 ppm", bufferLabel = "缓冲区  A — / B —", countersLabel = "4 CHANNELS / 2 DEVICES";
    private int sourceIndex;
    private double master = 0.5;
    private bool muted, loop = true;
    public Device? DeviceA { get => deviceA; set => Set(ref deviceA, value); }
    public Device? DeviceB { get => deviceB; set => Set(ref deviceB, value); }
    public int SourceIndex { get => sourceIndex; set => Set(ref sourceIndex, value); }
    public string WavPath { get => path; set => Set(ref path, value); }
    public bool Loop { get => loop; set => Set(ref loop, value); }
    public double Master { get => master; set { if (Set(ref master, Math.Clamp(value, 0, 1))) Edit(); } }
    public bool Muted { get => muted; set { if (Set(ref muted, value)) Edit(); } }
    public string StatusText { get => status; private set => Set(ref status, value); }
    public string LagLabel { get => lagLabel; private set => Set(ref lagLabel, value); }
    public string SyncLabel { get => syncLabel; private set => Set(ref syncLabel, value); }
    public string RatioLabel { get => ratioLabel; private set => Set(ref ratioLabel, value); }
    public string BufferLabel { get => bufferLabel; private set => Set(ref bufferLabel, value); }
    public string CountersLabel { get => countersLabel; private set => Set(ref countersLabel, value); }
    public bool CanConfigure => !busy && !running;
    public bool CanTransport => connected && !busy;
    public string TransportLabel => busy ? "正在处理…" : running ? "停止输出" : "启动音频引擎";
    public ConsoleModel()
    {
        foreach (var channel in Channels) channel.Edited += Edit;
        Restore();
        timer.Tick += async (_, _) => await PollAsync();
    }
    private void Edit() { pendingSettings = true; editedAt = DateTime.UtcNow; }
    private void State() { Changed(nameof(CanConfigure)); Changed(nameof(CanTransport)); Changed(nameof(TransportLabel)); }
    private object Settings() => new { command = "settings", master = Master, mute = Muted, channels = Channels.Select(c => new { gainDb = c.GainDb, delayMs = c.DelayMs }).ToArray() };
    private async Task SendSettingsAsync()
    {
        pendingSettings = false;
        try { await client.SendAsync(Settings()); }
        catch { pendingSettings = true; throw; }
    }
    public async Task InitializeAsync()
    {
        if (busy || closing) return;
        busy = true; State();
        try
        {
            if (!client.Connected)
            {
                try { await client.ConnectAsync(300); }
                catch (TimeoutException)
                {
                    var engine = Path.Combine(AppContext.BaseDirectory, "QuadSync.Engine.exe");
                    if (!File.Exists(engine)) throw new FileNotFoundException("未找到 QuadSync.Engine.exe，请运行 scripts/build.ps1 后从 dist 启动。");
                    if (ownedEngine is null || ownedEngine.HasExited)
                        ownedEngine = Process.Start(new ProcessStartInfo(engine) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = AppContext.BaseDirectory });
                    await client.ConnectAsync(6000);
                }
            }
            connected = true;
            var response = await client.SendAsync(new { command = "devices" });
            string a = DeviceA?.Id ?? savedA, b = DeviceB?.Id ?? savedB;
            var devices = response.GetProperty("devices").Deserialize<List<Device>>(JsonOptions) ?? [];
            Devices.Clear(); foreach (var d in devices) Devices.Add(d);
            DeviceA = Devices.FirstOrDefault(d => d.Id == a) ?? Devices.FirstOrDefault(d => d.SupportsFloat48) ?? Devices.FirstOrDefault();
            DeviceB = Devices.FirstOrDefault(d => d.Id == b && d.Id != DeviceA?.Id) ?? Devices.FirstOrDefault(d => d.SupportsFloat48 && d.Id != DeviceA?.Id);
            await SendSettingsAsync();
            StatusText = $"引擎已连接 · 找到 {Devices.Count} 台输出设备。选择两台不同声卡后启动。";
            timer.Start();
        }
        catch (Exception ex) { Fail(ex); }
        finally { busy = false; State(); }
    }
    public async Task ToggleAsync()
    {
        if (busy || closing) return;
        busy = true; State();
        try
        {
            if (running) { await client.SendAsync(new { command = "stop" }); running = false; StatusText = "输出已停止，设备已释放。"; }
            else
            {
                if (DeviceA is null || DeviceB is null || DeviceA.Id == DeviceB.Id) throw new InvalidOperationException("请选择两台不同的输出设备。");
                if (!DeviceA.SupportsFloat48 || !DeviceB.SupportsFloat48) throw new InvalidOperationException("所选设备不支持独占 Stereo / 48 kHz / Float32。请更换设备或检查驱动。" );
                if (SourceIndex == 1 && !File.Exists(WavPath)) throw new InvalidOperationException("请先选择可读取的 WAV 文件。");
                await SendSettingsAsync();
                await client.SendAsync(new { command = "start", deviceA = DeviceA.Id, deviceB = DeviceB.Id, source = new[] { "tone", "wav", "external" }[SourceIndex], path = WavPath, loop = Loop });
                running = true; StatusText = "正在输出 · 通道 0–1 → A，通道 2–3 → B。";
            }
        }
        catch (Exception ex) { Fail(ex); }
        finally { busy = false; State(); }
    }
    private async Task PollAsync()
    {
        if (polling || busy || closing || !connected) return;
        polling = true;
        try
        {
            if (pendingSettings && (DateTime.UtcNow - editedAt).TotalMilliseconds >= 70) await SendSettingsAsync();
            var data = await client.SendAsync(new { command = "status" });
            bool wasRunning = running; running = data.GetProperty("running").GetBoolean();
            if (wasRunning != running) State();
            int i = 0; foreach (var peak in data.GetProperty("peaks").EnumerateArray()) Channels[i++].SetPeak(running ? peak.GetDouble() : 0);
            double lag = data.GetProperty("lagFrames").GetDouble();
            LagLabel = $"{lag:+0.0;-0.0;0.0} frames";
            SyncLabel = running ? (data.GetProperty("correcting").GetBoolean() ? "自适应补偿中" : "监测中 · 64 帧触发") : "等待音频时钟";
            RatioLabel = $"补偿  {(data.GetProperty("ratio").GetDouble() - 1) * 1e6:+0.0;-0.0;0.0} ppm";
            BufferLabel = $"缓冲区  A {data.GetProperty("bufferA")} / B {data.GetProperty("bufferB")} frames";
            CountersLabel = $"补零 {data.GetProperty("underrunFrames")} 帧  ·  削波 {data.GetProperty("clippedSamples")} 样本";
            string error = data.GetProperty("error").GetString() ?? "";
            if (error.Length > 0) StatusText = "音频设备错误：" + error;
            else if (running && data.GetProperty("eof").GetBoolean()) StatusText = "WAV 播放结束 · 当前输出静音，可停止后重新播放。";
        }
        catch (Exception ex) { Fail(ex); }
        finally { polling = false; }
    }
    private void Fail(Exception ex)
    {
        StatusText = ex is OperationCanceledException ? "引擎响应超时。请点击刷新设备重新连接。" : ex.Message;
        connected = client.Connected;
        if (!connected) { running = false; timer.Stop(); StatusText += "  点击刷新设备重连。"; }
        State();
    }
    private sealed record ChannelSettings(double GainDb, double DelayMs, string Unit);
    private sealed record SavedSettings(string DeviceA, string DeviceB, double Master, bool Muted, int SourceIndex, string WavPath, bool Loop, ChannelSettings[] Channels);
    private void Restore()
    {
        if (!File.Exists(settingsPath)) return;
        try
        {
            var s = JsonSerializer.Deserialize<SavedSettings>(File.ReadAllText(settingsPath), JsonOptions);
            if (s is null || s.Channels.Length != 4) return;
            savedA = s.DeviceA; savedB = s.DeviceB; Master = double.IsFinite(s.Master) ? s.Master : 0.5; Muted = s.Muted;
            SourceIndex = Math.Clamp(s.SourceIndex, 0, 2); WavPath = s.WavPath; Loop = s.Loop;
            for (int i = 0; i < 4; i++) Channels[i].Restore(s.Channels[i].GainDb, s.Channels[i].DelayMs, s.Channels[i].Unit);
        }
        catch (Exception ex) { StatusText = "配置读取失败，使用可用设置：" + ex.Message; }
    }
    public async Task CloseAsync()
    {
        closing = true; timer.Stop();
        try
        {
            var s = new SavedSettings(DeviceA?.Id ?? savedA, DeviceB?.Id ?? savedB, Master, Muted, SourceIndex, WavPath, Loop,
                Channels.Select(c => new ChannelSettings(c.GainDb, c.DelayMs, c.Unit)).ToArray());
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
            string temporary = settingsPath + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(s, JsonOptions)); File.Move(temporary, settingsPath, true);
        }
        catch (Exception ex) { MessageBox.Show("无法保存设置：" + ex.Message, "QuadSyncAudio"); }
        if (ownedEngine is not null)
        {
            try { if (client.Connected) await client.SendAsync(new { command = "shutdown" }); }
            catch (Exception) { /* Process exit below releases any exclusive endpoints. */ }
            if (!ownedEngine.HasExited)
            {
                using var timeout = new CancellationTokenSource(2000);
                try { await ownedEngine.WaitForExitAsync(timeout.Token); }
                catch (OperationCanceledException) { ownedEngine.Kill(); }
            }
            ownedEngine.Dispose();
        }
        client.Dispose();
    }
}
