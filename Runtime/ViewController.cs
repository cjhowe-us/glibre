namespace GLI.Runtime;

public class ViewController
#if MACOS
: NSViewController
#elif IOS
: UIViewController
#endif
{
    private readonly TriangleRenderer TriangleRenderer = new();

    public override void LoadView()
    {
        base.LoadView();

        View = TriangleRenderer.View;
    }

}
