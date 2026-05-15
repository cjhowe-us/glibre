using Glibre.Runtime.Hosting;

using var host = new SdlRuntimeHost(new RuntimeWindowOptions
{
    Title = "glibre runtime",
});

host.Run();
