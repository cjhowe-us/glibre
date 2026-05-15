using System;
using System.Runtime.InteropServices;
using Avalonia.Controls;
using Avalonia.Platform;
using Avalonia.Threading;
using Editor.Rendering;
using SharpMetal.Metal;
using SharpMetal.ObjectiveCCore;
using SharpMetal.QuartzCore;

namespace Editor.Views;

/// <summary>
/// Avalonia host that mounts a native <c>NSView</c> backed by a <see cref="CAMetalLayer"/>
/// and drives a <see cref="TriangleRenderer"/> via a UI-thread timer. macOS only;
/// renders nothing on other platforms.
/// </summary>
public sealed class MetalView : NativeControlHost
{
    [StructLayout(LayoutKind.Sequential)]
    private struct CGSize { public double Width; public double Height; }

    [StructLayout(LayoutKind.Sequential)]
    private struct CGRect { public double X, Y, Width, Height; }

    private const string ObjC = "/usr/lib/libobjc.dylib";

    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    private static extern IntPtr SendInitWithFrame(IntPtr receiver, IntPtr selector, CGRect frame);

    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    private static extern void SendSetDrawableSize(IntPtr receiver, IntPtr selector, CGSize size);

    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    private static extern double SendReturningDouble(IntPtr receiver, IntPtr selector);

    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    private static extern CGRect SendReturningRectArm64(IntPtr receiver, IntPtr selector);

    [DllImport(ObjC, EntryPoint = "objc_msgSend_stret")]
    private static extern void SendReturningRectIntel(out CGRect outRect, IntPtr receiver, IntPtr selector);

    private TriangleRenderer? _renderer;
    private CAMetalLayer _layer;
    private DispatcherTimer? _timer;
    private IntPtr _nsView;

    protected override IPlatformHandle CreateNativeControlCore(IPlatformHandle parent)
    {
        if (!OperatingSystem.IsMacOS())
            return base.CreateNativeControlCore(parent);

        var device = MTLDevice.CreateSystemDefaultDevice();

        _layer = AllocCAMetalLayer();
        _layer.Device = device;
        _layer.PixelFormat = MTLPixelFormat.BGRA8Unorm;
        _layer.FramebufferOnly = true;

        _nsView = AllocLayerHostingNSView(_layer);

        _renderer = new TriangleRenderer(device, _layer);

        _timer = new DispatcherTimer(TimeSpan.FromMilliseconds(16), DispatcherPriority.Render, OnTick);
        _timer.Start();

        return new PlatformHandle(_nsView, "NSView");
    }

    protected override void DestroyNativeControlCore(IPlatformHandle control)
    {
        _timer?.Stop();
        _timer = null;
        _renderer?.Dispose();
        _renderer = null;

        if (OperatingSystem.IsMacOS() && _nsView != IntPtr.Zero)
        {
            ObjectiveC.objc_msgSend(_nsView, new Selector("release"));
            _nsView = IntPtr.Zero;
        }
        else
        {
            base.DestroyNativeControlCore(control);
        }
    }

    private void OnTick(object? sender, EventArgs e)
    {
        if (!OperatingSystem.IsMacOS() || _renderer is null) return;
        SyncDrawableSize();
        _renderer.Render();
    }

    private void SyncDrawableSize()
    {
        // CAMetalLayer.drawableSize is in pixels; bounds is in points. Multiply
        // by contentsScale (backing scale factor) so retina displays render sharp.
        var bounds = GetBounds(_nsView);
        var scale = SendReturningDouble(_layer, new Selector("contentsScale"));
        if (scale <= 0) scale = 1.0;
        var pixelSize = new CGSize
        {
            Width = bounds.Width * scale,
            Height = bounds.Height * scale,
        };
        SendSetDrawableSize(_layer, new Selector("setDrawableSize:"), pixelSize);
    }

    private static CGRect GetBounds(IntPtr nsView)
    {
        var sel = new Selector("bounds");
        // x86_64 returns structs > 16 bytes via objc_msgSend_stret; arm64 uses
        // plain objc_msgSend with an indirect-return register (x8).
        if (RuntimeInformation.OSArchitecture == Architecture.X64)
        {
            SendReturningRectIntel(out var r, nsView, sel);
            return r;
        }
        return SendReturningRectArm64(nsView, sel);
    }

    private static CAMetalLayer AllocCAMetalLayer()
    {
        var cls = ObjectiveC.objc_getClass("CAMetalLayer");
        var instance = ObjectiveC.IntPtr_objc_msgSend(cls, new Selector("alloc"));
        instance = ObjectiveC.IntPtr_objc_msgSend(instance, new Selector("init"));
        return new CAMetalLayer(instance);
    }

    private static IntPtr AllocLayerHostingNSView(CAMetalLayer layer)
    {
        var cls = ObjectiveC.objc_getClass("NSView");
        var instance = ObjectiveC.IntPtr_objc_msgSend(cls, new Selector("alloc"));
        instance = SendInitWithFrame(instance, new Selector("initWithFrame:"), default);
        ObjectiveC.objc_msgSend(instance, new Selector("setWantsLayer:"), true);
        ObjectiveC.objc_msgSend(instance, new Selector("setLayer:"), (IntPtr)layer);
        return instance;
    }
}
