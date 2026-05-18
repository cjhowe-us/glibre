using Metal;
using MetalKit;

namespace GLI.Runtime;

/// <summary>
/// Renders a single RGB-interpolated triangle into a <see cref="MTKView"/>.
/// Owns the Metal command queue and pipeline state for the demo.
/// </summary>
public sealed class TriangleRenderer : MTKViewDelegate
{
    private const string ShaderSource = """
        # include <metal_stdlib>
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

    internal readonly MTKView View = new(new CGRect
    {
        X = 0,
        Y = 0,
        Width = 1440,
        Height = 960
    }, MTLDevice.SystemDefault);
    private IMTLDevice? Device => View.Device;

    private readonly IMTL4CommandQueue? _queue;
    private readonly IMTL4CommandAllocator? _commandAlloc;
    private readonly IMTL4CommandBuffer? _commandBuffer;
    private readonly IMTL4CommandBuffer[] _commandBuffers = [];
    private readonly IMTLRenderPipelineState? _pipeline;

    public TriangleRenderer()
    {
        View.Delegate = this;
        _queue = Device?.CreateMTL4CommandQueue();
        _commandAlloc = Device?.CreateCommandAllocator();
        _commandBuffer = Device?.CreateCommandBuffer();
        _commandBuffers = _commandBuffer == null ? [] : [_commandBuffer];

        if (Device != null)
        {
            var library = Device.CreateLibrary(ShaderSource, new MTLCompileOptions(), out var error)
                ?? throw new InvalidOperationException(
                    $"Metal shader compile failed: {error.LocalizedDescription}");
            var vertexFn = library.CreateFunction("vertexMain");
            var fragmentFn = library.CreateFunction("fragmentMain");

            var pipelineDesc = new MTLRenderPipelineDescriptor
            {
                VertexFunction = vertexFn,
                FragmentFunction = fragmentFn,
                Label = "TriangleRenderer",
            };
            var colorAttachment = pipelineDesc.ColorAttachments[0];
            colorAttachment.PixelFormat = View.ColorPixelFormat;

            _pipeline = Device?.CreateRenderPipelineState(pipelineDesc, out error)
                ?? throw new InvalidOperationException(
                    $"Metal pipeline build failed: {error.LocalizedDescription}");
        }
    }

    public override void Draw(MTKView view)
    {
        var drawable = view.CurrentDrawable;
        if (drawable == null) return;

        var passDesc = new MTL4RenderPassDescriptor();
        var color0 = passDesc.ColorAttachments[0];
        color0.Texture = drawable.Texture;
        color0.LoadAction = MTLLoadAction.Clear;
        color0.StoreAction = MTLStoreAction.Store;
        color0.ClearColor = new MTLClearColor { Red = 0.08, Green = 0.08, Blue = 0.10, Alpha = 1.0 };

        if (_commandAlloc == null || _commandBuffer == null || _queue == null || _pipeline == null)
        {
            return;
        }

        _commandBuffer.BeginCommandBuffer(_commandAlloc);

        var encoder = _commandBuffer.CreateRenderCommandEncoder(passDesc);
        encoder?.SetRenderPipelineState(_pipeline);
        encoder?.DrawPrimitives(MTLPrimitiveType.Triangle, 0, 3);
        encoder?.EndEncoding();

        _commandBuffer.EndCommandBuffer();

        _queue.Commit([_commandBuffer]);

        drawable.Present();

        _commandAlloc.Reset();

    }

    public override void DrawableSizeWillChange(MTKView view, CGSize size)
    {
    }

}
