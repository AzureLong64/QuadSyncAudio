using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;

namespace QuadSync.Control;
public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    protected void Changed([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    { if (EqualityComparer<T>.Default.Equals(field, value)) return false; field = value; Changed(name); return true; }
}
public sealed class Device
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string ContainerId { get; set; } = "";
    public bool SupportsFloat48 { get; set; }
    [JsonIgnore] public string Label => Name + (SupportsFloat48 ? "" : "  · 不支持独占 Float32");
}
public sealed class Channel : Observable
{
    public string Name { get; init; } = "";
    public string Subtitle { get; init; } = "";
    public string Number { get; init; } = "";
    public string[] Units { get; } = ["ms", "m"];
    public event Action? Edited;
    private double gain, delayMs, meter;
    private string unit = "ms", peakText = "−∞ dBFS";
    public double GainDb { get => gain; set { if (Set(ref gain, Math.Clamp(value, -60, 6))) Edited?.Invoke(); } }
    public double DelayValue
    {
        get => unit == "ms" ? delayMs : delayMs * 343 / 1000;
        set
        {
            if (!double.IsFinite(value) || value < 0 || value > (unit == "ms" ? 100 : 34.3))
                throw new ArgumentOutOfRangeException(nameof(value), "延迟范围为 0–100 ms / 0–34.3 m");
            delayMs = unit == "ms" ? value : value * 1000 / 343;
            Changed(); Changed(nameof(DelayCaption)); Edited?.Invoke();
        }
    }
    public string Unit { get => unit; set { if (Set(ref unit, value)) { Changed(nameof(DelayValue)); Changed(nameof(DelayCaption)); Edited?.Invoke(); } } }
    public string DelayCaption => $"{delayMs * 48:0.0} samples · {delayMs:0.0} ms";
    public double DelayMs => delayMs;
    public double Meter { get => meter; private set => Set(ref meter, value); }
    public string PeakText { get => peakText; private set => Set(ref peakText, value); }
    public void SetPeak(double amplitude)
    {
        double db = amplitude > 0.000001 ? 20 * Math.Log10(amplitude) : -120;
        Meter = Math.Max(Math.Clamp((db + 60) / 60, 0, 1) * 220, Meter - 9);
        PeakText = db < -60 ? "−∞ dBFS" : $"{db:0.0} dBFS";
    }
    public void Restore(double db, double ms, string u) { GainDb = db; Unit = "ms"; DelayValue = ms; Unit = u is "m" ? "m" : "ms"; }
}
