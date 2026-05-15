using System.Runtime.InteropServices;
using Glibre.Rendering;
using SDL3;
using SharpMetal.Metal;
using SharpMetal.ObjectiveCCore;
using SharpMetal.QuartzCore;

namespace Glibre.Runtime.Hosting;

/// <summary>
/// Standalone SDL3-hosted window driving a <see cref="TriangleRenderer"/> on a
/// <c>CAMetalLayer</c> obtained via <c>SDL_MetalCreateView</c>. SDL owns the
/// window and event loop; rendering goes through SharpMetal so the editor and
/// the runtime share the same renderer (the editor drives it against its own
/// Avalonia-supplied NSView, without SDL).
/// </summary>
public sealed class SdlRuntimeHost : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    private struct CGSize { public double Width; public double Height; }

    [DllImport("/usr/lib/libobjc.dylib", EntryPoint = "objc_msgSend")]
    private static extern void SendSetDrawableSize(IntPtr receiver, IntPtr selector, CGSize size);

    private readonly RuntimeWindowOptions _options;
    private IntPtr _window;
    private IntPtr _metalView;
    private MTLDevice _device;
    private CAMetalLayer _layer;
    private TriangleRenderer? _triangle;
    private int _lastPixelWidth;
    private int _lastPixelHeight;
    private bool _initialized;
    private bool _disposed;

    public SdlRuntimeHost(RuntimeWindowOptions? options = null)
    {
        _options = options ?? new RuntimeWindowOptions();
    }

    /// <summary>Runs a blocking event/render loop until the window is closed.</summary>
    public void Run()
    {
        Initialize();
        while (Tick())
        {
        }
    }

    /// <summary>
    /// Drains pending events and renders a single frame. Returns <c>false</c>
    /// when a quit event has been received. Callers embedding the host in
    /// another UI framework should call this from their own frame timer.
    /// </summary>
    public bool Tick()
    {
        EnsureInitialized();

        while (SDL.PollEvent(out var ev))
        {
            if ((SDL.EventType)ev.Type == SDL.EventType.Quit)
            {
                return false;
            }
        }

        SyncDrawableSize();
        _triangle?.Render();
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

        _window = CreateWindow();
        if (_window == IntPtr.Zero)
        {
            var error = SDL.GetError();
            SDL.Quit();
            throw new InvalidOperationException($"SDL_CreateWindow failed: {error}");
        }

        _metalView = SDL.MetalCreateView(_window);
        if (_metalView == IntPtr.Zero)
        {
            var error = SDL.GetError();
            SDL.DestroyWindow(_window);
            _window = IntPtr.Zero;
            SDL.Quit();
            throw new InvalidOperationException($"SDL_Metal_CreateView failed: {error}");
        }

        var layerPtr = SDL.MetalGetLayer(_metalView);
        if (layerPtr == IntPtr.Zero)
        {
            var error = SDL.GetError();
            throw new InvalidOperationException($"SDL_Metal_GetLayer failed: {error}");
        }

        _device = MTLDevice.CreateSystemDefaultDevice();
        _layer = new CAMetalLayer(layerPtr)
        {
            Device = _device,
            PixelFormat = MTLPixelFormat.BGRA8Unorm,
            FramebufferOnly = true,
        };

        _triangle = new TriangleRenderer(_device, _layer);

        _initialized = true;
    }

    private IntPtr CreateWindow()
    {
        var flags = SDL.WindowFlags.Metal | SDL.WindowFlags.HighPixelDensity;
        if (_options.Resizable)
        {
            flags |= SDL.WindowFlags.Resizable;
        }
        return SDL.CreateWindow(_options.Title, _options.Width, _options.Height, flags);
    }

    private void SyncDrawableSize()
    {
        if (_window == IntPtr.Zero) return;
        SDL.GetWindowSizeInPixels(_window, out var w, out var h);
        if (w <= 0 || h <= 0) return;
        if (w == _lastPixelWidth && h == _lastPixelHeight) return;

        // CAMetalLayer.drawableSize is in pixels (CGSize, two doubles). SharpMetal
        // surfaces it as an opaque IntPtr, so call setDrawableSize: directly via
        // objc_msgSend with a CGSize struct.
        SendSetDrawableSize(_layer, new Selector("setDrawableSize:"), new CGSize { Width = w, Height = h });
        _lastPixelWidth = w;
        _lastPixelHeight = h;
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

        _triangle?.Dispose();
        _triangle = null;

        if (_metalView != IntPtr.Zero)
        {
            SDL.MetalDestroyView(_metalView);
            _metalView = IntPtr.Zero;
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
