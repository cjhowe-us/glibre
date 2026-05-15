using SDL3;

namespace Glibre.Runtime.Hosting;

public sealed class SdlRuntimeHost : IDisposable
{
    private const string MetalDriver = "metal";

    private readonly RuntimeWindowOptions _options;
    private IntPtr _window;
    private IntPtr _renderer;
    private bool _initialized;
    private bool _disposed;

    public SdlRuntimeHost(RuntimeWindowOptions? options = null)
    {
        _options = options ?? new RuntimeWindowOptions();
    }

    public void Run()
    {
        Initialize();

        while (PumpFrame())
        {
        }
    }

    public bool PumpFrame()
    {
        EnsureInitialized();

        while (SDL.PollEvent(out var ev))
        {
            if ((SDL.EventType)ev.Type == SDL.EventType.Quit)
            {
                return false;
            }
        }

        SDL.RenderClear(_renderer);
        SDL.RenderPresent(_renderer);
        return true;
    }

    private void Initialize()
    {
        if (_initialized)
        {
            return;
        }

        if (!SDL.Init(SDL.InitFlags.Video))
        {
            throw new InvalidOperationException($"SDL_Init failed: {SDL.GetError()}");
        }

        var flags = SDL.WindowFlags.Metal;
        if (_options.Resizable)
        {
            flags |= SDL.WindowFlags.Resizable;
        }

        _window = SDL.CreateWindow(_options.Title, _options.Width, _options.Height, flags);
        if (_window == IntPtr.Zero)
        {
            var error = SDL.GetError();
            SDL.Quit();
            throw new InvalidOperationException($"SDL_CreateWindow failed: {error}");
        }

        _renderer = SDL.CreateRenderer(_window, MetalDriver);
        if (_renderer == IntPtr.Zero)
        {
            var error = SDL.GetError();
            SDL.DestroyWindow(_window);
            _window = IntPtr.Zero;
            SDL.Quit();
            throw new InvalidOperationException($"SDL_CreateRenderer(metal) failed: {error}");
        }

        SDL.SetRenderVSync(_renderer, _options.VSync ? 1 : 0);

        var color = _options.ClearColor;
        SDL.SetRenderDrawColor(_renderer, color.R, color.G, color.B, color.A);

        _initialized = true;
    }

    private void EnsureInitialized()
    {
        if (!_initialized)
        {
            Initialize();
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        if (_renderer != IntPtr.Zero)
        {
            SDL.DestroyRenderer(_renderer);
            _renderer = IntPtr.Zero;
        }

        if (_window != IntPtr.Zero)
        {
            SDL.DestroyWindow(_window);
            _window = IntPtr.Zero;
        }

        if (_initialized)
        {
            SDL.Quit();
        }

        _disposed = true;
        _initialized = false;
    }
}
