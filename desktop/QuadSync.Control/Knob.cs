using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace QuadSync.Control;
public sealed class Knob : FrameworkElement
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(nameof(Value), typeof(double), typeof(Knob),
        new FrameworkPropertyMetadata(0.5, FrameworkPropertyMetadataOptions.AffectsRender | FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, null, (_, v) => Math.Clamp((double)v, 0, 1)));
    public double Value { get => (double)GetValue(ValueProperty); set => SetCurrentValue(ValueProperty, value); }
    private Point origin;
    private double initial;
    public Knob() { Focusable = true; Cursor = Cursors.SizeNS; ToolTip = "上下拖动、滚轮或方向键调节总音量；Home 静音量，End 最大音量"; }
    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        double radius = Math.Min(ActualWidth, ActualHeight) / 2 - 12;
        var center = new Point(ActualWidth / 2, ActualHeight / 2);
        var accent = new SolidColorBrush(Color.FromRgb(71, 223, 205));
        dc.DrawEllipse(new SolidColorBrush(Color.FromRgb(13, 20, 32)), new Pen(new SolidColorBrush(Color.FromRgb(44, 60, 81)), 1), center, radius - 12, radius - 12);
        for (int i = 0; i <= 50; i++)
        {
            double angle = (135 + 270 * i / 50.0) * Math.PI / 180;
            Point At(double r) => new(center.X + r * Math.Cos(angle), center.Y + r * Math.Sin(angle));
            dc.DrawLine(new Pen(i <= Value * 50 ? accent : new SolidColorBrush(Color.FromRgb(44, 60, 81)), 3), At(radius - 4), At(radius + 3));
        }
        var text = new FormattedText($"{Value * 100:0}", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Segoe UI Semibold"), 36, Brushes.White, VisualTreeHelper.GetDpi(this).PixelsPerDip);
        dc.DrawText(text, new Point(center.X - text.Width / 2, center.Y - text.Height / 2 - 5));
        var label = new FormattedText("MASTER %", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Segoe UI"), 10, accent, VisualTreeHelper.GetDpi(this).PixelsPerDip);
        dc.DrawText(label, new Point(center.X - label.Width / 2, center.Y + 24));
        if (IsKeyboardFocused) dc.DrawEllipse(null, new Pen(accent, 1), center, radius + 8, radius + 8);
    }
    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e) { Focus(); origin = e.GetPosition(this); initial = Value; CaptureMouse(); e.Handled = true; }
    protected override void OnMouseMove(MouseEventArgs e) { if (IsMouseCaptured) Value = initial + (origin.Y - e.GetPosition(this).Y) / 180; }
    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e) { ReleaseMouseCapture(); }
    protected override void OnMouseWheel(MouseWheelEventArgs e) { Value += e.Delta / 120.0 * 0.01; e.Handled = true; }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key is Key.Up or Key.Right) Value += 0.01;
        else if (e.Key is Key.Down or Key.Left) Value -= 0.01;
        else if (e.Key == Key.Home) Value = 0;
        else if (e.Key == Key.End) Value = 1;
        else { base.OnKeyDown(e); return; }
        e.Handled = true;
    }
}
