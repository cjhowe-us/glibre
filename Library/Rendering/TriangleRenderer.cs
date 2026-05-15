using SharpMetal.Foundation;
using SharpMetal.Metal;
using SharpMetal.QuartzCore;

namespace Glibre.Rendering;

/// <summary>
/// Renders a single RGB-interpolated triangle into a <see cref="CAMetalLayer"/>.
/// Owns the Metal command queue and pipeline state for the demo.
/// </summary>
public sealed class TriangleRenderer : IDisposable
{
    private const string ShaderSource = """
        #include <metal_stdlib>
        using namespace metal;

        struct VSOut {
            float4 position [[position]];
            float4 color;
        };

        vertex VSOut vertexMain(uint vid [[vertex_id]]) {
            float2 p[3] = { float2( 0.0,  0.6), float2(-0.6, -0.6), float2( 0.6, -0.6) };
            float4 c[3] = { float4(1,0,0,1),    float4(0,1,0,1),     float4(0,0,1,1)    };
            VSOut o;
            o.position = float4(p[vid], 0.0, 1.0);
            o.color    = c[vid];
            return o;
        }

        fragment float4 fragmentMain(VSOut in [[stage_in]]) {
            return in.color;
        }
    """;

    private readonly MTLCommandQueue _queue;
    private readonly MTLRenderPipelineState _pipeline;
    private readonly CAMetalLayer _layer;
    private bool _disposed;

    public TriangleRenderer(MTLDevice device, CAMetalLayer layer)
    {
        _layer = layer;
        _queue = device.NewCommandQueue();

        NSError error = new();
        var library = device.NewLibrary(NSString.String(ShaderSource), new MTLCompileOptions(), ref error);
        if (library.NativePtr == IntPtr.Zero)
            throw new InvalidOperationException(
                $"Metal shader compile failed: {error.LocalizedDescription.ToString()}");

        var vertexFn = library.NewFunction(NSString.String("vertexMain"));
        var fragmentFn = library.NewFunction(NSString.String("fragmentMain"));

        var pipelineDesc = new MTLRenderPipelineDescriptor
        {
            VertexFunction = vertexFn,
            FragmentFunction = fragmentFn,
            Label = NSString.String("TriangleRenderer"),
        };
        var colorAttachment = pipelineDesc.ColorAttachments.Object(0);
        colorAttachment.PixelFormat = _layer.PixelFormat;

        error = new NSError();
        _pipeline = device.NewRenderPipelineState(pipelineDesc, ref error);
        if (_pipeline.NativePtr == IntPtr.Zero)
            throw new InvalidOperationException(
                $"Metal pipeline build failed: {error.LocalizedDescription.ToString()}");
    }

    public void Render()
    {
        if (_disposed) return;

        var drawable = _layer.NextDrawable;
        if (drawable.NativePtr == IntPtr.Zero) return; // surface not ready

        var passDesc = new MTLRenderPassDescriptor();
        var color0 = passDesc.ColorAttachments.Object(0);
        color0.Texture = drawable.Texture;
        color0.LoadAction = MTLLoadAction.Clear;
        color0.StoreAction = MTLStoreAction.Store;
        color0.ClearColor = new MTLClearColor { red = 0.08, green = 0.08, blue = 0.10, alpha = 1.0 };

        var cmd = _queue.CommandBuffer();
        var encoder = cmd.RenderCommandEncoder(passDesc);
        encoder.SetRenderPipelineState(_pipeline);
        encoder.DrawPrimitives(MTLPrimitiveType.Triangle, 0, 3);
        encoder.EndEncoding();

        cmd.PresentDrawable(drawable);
        cmd.Commit();
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        // Metal objects are reference-counted by the runtime; SharpMetal structs
        // wrap native handles, and GC + autorelease pools handle cleanup.
    }
}
