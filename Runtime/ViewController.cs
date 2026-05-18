namespace GLI.Runtime;

public class ViewController
#if MACOS
: NSViewController
#elif IOS
: UIViewController
#endif
{
    private readonly TriangleRenderer _triangleRenderer = new();

    public override void LoadView()
    {
        base.LoadView();

        View = _triangleRenderer.View;
    }

}
