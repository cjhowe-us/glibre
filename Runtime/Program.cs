using Glibre.Runtime.Hosting;

using var host = new SdlRuntimeHost(new RuntimeWindowOptions
{
    Title = "glibre runtime",
    ClearColor = ClearColor.CornflowerBlue,
});

host.Run();
