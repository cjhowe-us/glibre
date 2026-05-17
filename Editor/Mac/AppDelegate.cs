using GLI.Runtime;

namespace GLI.Editor.Mac;

[Register("AppDelegate")]
public class AppDelegate : NSApplicationDelegate
{
    private static readonly CGRect frame = new() { X = 0, Y = 0, Width = 1440, Height = 960 };
    private readonly ViewController ViewController = new();
    private readonly NSWindow Window = new(
            frame,
            NSWindowStyle.Titled | NSWindowStyle.Resizable | NSWindowStyle.Closable | NSWindowStyle.Miniaturizable,
            NSBackingStore.Buffered,
            true)
    {
        Title = "Glibre",
    };

    public override void DidFinishLaunching(NSNotification notification)
    {
        Window.ContentViewController = ViewController;
        Window.IsVisible = true;
        Window.MakeKeyAndOrderFront(null);
    }

    public override bool ApplicationShouldTerminateAfterLastWindowClosed(NSApplication sender)
    {
        return true;
    }
}
