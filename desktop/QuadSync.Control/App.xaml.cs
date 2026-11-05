using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace QuadSync.Control;
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var window = new MainWindow();
        if (e.Args.Length == 2 && e.Args[0] == "--render")
        {
            var content = (System.Windows.Controls.ScrollViewer)window.Content;
            window.Content = null;
            content.DataContext = window.DataContext;
            content.Background = window.Background;
            content.Measure(new Size(1240, 850));
            content.Arrange(new Rect(0, 0, 1240, 850));
            content.UpdateLayout();
            var bitmap = new RenderTargetBitmap(1240, 850, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(content);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using var file = File.Create(e.Args[1]); encoder.Save(file);
            Shutdown();
            return;
        }
        window.Show();
    }
}
