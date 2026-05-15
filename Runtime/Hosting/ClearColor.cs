namespace Glibre.Runtime.Hosting;

public readonly record struct ClearColor(byte R, byte G, byte B, byte A = 255)
{
    public static ClearColor CornflowerBlue { get; } = new(100, 149, 237);
}
