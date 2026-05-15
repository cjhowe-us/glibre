namespace Glibre.Runtime.Hosting;

public sealed record RuntimeWindowOptions
{
    public string Title { get; init; } = "glibre";

    public int Width { get; init; } = 1280;

    public int Height { get; init; } = 720;

    public bool Resizable { get; init; } = true;
}
