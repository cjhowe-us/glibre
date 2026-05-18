using GLI.Runtime;

namespace GLI.Editor.Mac;

[Register("AppDelegate")]
public class AppDelegate : NSApplicationDelegate
{
    private static readonly CGRect Frame = new() { X = 0, Y = 0, Width = 1440, Height = 960 };
    private readonly ViewController _viewController = new();
    private readonly NSWindow _window = new(
            Frame,
            NSWindowStyle.Titled | NSWindowStyle.Resizable | NSWindowStyle.Closable | NSWindowStyle.Miniaturizable,
            NSBackingStore.Buffered,
            true)
    {
        Title = "Glibre",
    };

    public override void DidFinishLaunching(NSNotification notification)
    {
        _window.ContentViewController = _viewController;
        _window.SetIsVisible(true);
        _window.MakeKeyAndOrderFront(null);
    }

    public override bool ApplicationShouldTerminateAfterLastWindowClosed(NSApplication sender)
    {
        return true;
    }
}
