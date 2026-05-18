using System.Runtime.InteropServices;
using SDL;
using Vortice.ShaderCompiler;
using Vortice.Vulkan;
using static SDL.SDL3;
using static Vortice.Vulkan.Vulkan;

namespace GLI.Runtime;

/// <summary>
/// Renders a single RGB-interpolated triangle into an SDL3 window via Vulkan.
/// Owns the Vulkan instance, device, swapchain and pipeline for the demo.
/// </summary>
public sealed unsafe class TriangleRenderer : IDisposable
{
    private const int MaxFramesInFlight = 2;

    private const string VertexShaderSource = """
        #version 450
        layout(location = 0) out vec4 vColor;
        void main() {
            vec2 p[3] = vec2[3](
                vec2( 0.0,  0.6),
                vec2(-0.6, -0.6),
                vec2( 0.6, -0.6));
            vec4 c[3] = vec4[3](
                vec4(1, 0, 0, 1),
                vec4(0, 1, 0, 1),
                vec4(0, 0, 1, 1));
            gl_Position = vec4(p[gl_VertexIndex].x, -p[gl_VertexIndex].y, 0.0, 1.0);
            vColor = c[gl_VertexIndex];
        }
        """;

    private const string FragmentShaderSource = """
        #version 450
        layout(location = 0) in  vec4 vColor;
        layout(location = 0) out vec4 outColor;
        void main() { outColor = vColor; }
        """;

    private readonly SDL_Window* _window;

    private VkInstance _instance;
    private VkInstanceApi _instanceApi = null!;
    private nint _metalView;
    private VkSurfaceKHR _surface;
    private VkPhysicalDevice _physicalDevice;
    private VkDevice _device;
    private VkDeviceApi _deviceApi = null!;
    private uint _graphicsQueueFamily;
    private VkQueue _graphicsQueue;

    private VkSwapchainKHR _swapchain;
    private VkFormat _swapchainFormat;
    private VkExtent2D _swapchainExtent;
    private VkImage[] _swapchainImages = [];
    private VkImageView[] _swapchainImageViews = [];
    private VkFramebuffer[] _framebuffers = [];

    private VkRenderPass _renderPass;
    private VkPipelineLayout _pipelineLayout;
    private VkPipeline _pipeline;

    private VkCommandPool _commandPool;
    private readonly VkCommandBuffer[] _commandBuffers = new VkCommandBuffer[MaxFramesInFlight];
    private readonly VkSemaphore[] _imageAvailable = new VkSemaphore[MaxFramesInFlight];
    private readonly VkSemaphore[] _renderFinished = new VkSemaphore[MaxFramesInFlight];
    private readonly VkFence[] _inFlight = new VkFence[MaxFramesInFlight];

    private int _currentFrame;
    private bool _resizeRequested;
    private bool _disposed;

    public TriangleRenderer(SDL_Window* window)
    {
        _window = window;

        vkInitialize().CheckResult();
        CreateInstance();
        _instanceApi = GetApi(_instance);
        CreateSurface();
        PickPhysicalDevice();
        CreateDeviceAndQueue();
        _deviceApi = GetApi(_instance, _device);
        CreateSwapchain();
        CreateRenderPass();
        CreatePipeline();
        CreateFramebuffers();
        CreateCommandPool();
        CreateSyncObjects();
    }

    public void OnResize() => _resizeRequested = true;

    public void DrawFrame()
    {
        if (_resizeRequested)
        {
            RecreateSwapchain();
            _resizeRequested = false;
        }

        var fence = _inFlight[_currentFrame];
        _deviceApi.vkWaitForFences(fence, true, ulong.MaxValue).CheckResult();

        uint imageIndex;
        var acquireResult = _deviceApi.vkAcquireNextImageKHR(
            _swapchain, ulong.MaxValue,
            _imageAvailable[_currentFrame], VkFence.Null, out imageIndex);
        if (acquireResult == VkResult.ErrorOutOfDateKHR)
        {
            RecreateSwapchain();
            return;
        }
        if (acquireResult != VkResult.Success && acquireResult != VkResult.SuboptimalKHR)
        {
            acquireResult.CheckResult();
        }

        _deviceApi.vkResetFences(fence).CheckResult();

        var cmd = _commandBuffers[_currentFrame];
        _deviceApi.vkResetCommandBuffer(cmd, 0).CheckResult();
        RecordCommandBuffer(cmd, imageIndex);

        var waitSemaphore = _imageAvailable[_currentFrame];
        var signalSemaphore = _renderFinished[_currentFrame];
        var waitStage = VkPipelineStageFlags.ColorAttachmentOutput;

        var submit = new VkSubmitInfo
        {
            waitSemaphoreCount = 1,
            pWaitSemaphores = &waitSemaphore,
            pWaitDstStageMask = &waitStage,
            commandBufferCount = 1,
            pCommandBuffers = &cmd,
            signalSemaphoreCount = 1,
            pSignalSemaphores = &signalSemaphore,
        };
        _deviceApi.vkQueueSubmit(_graphicsQueue, 1, &submit, fence).CheckResult();

        var swapchain = _swapchain;
        var present = new VkPresentInfoKHR
        {
            waitSemaphoreCount = 1,
            pWaitSemaphores = &signalSemaphore,
            swapchainCount = 1,
            pSwapchains = &swapchain,
            pImageIndices = &imageIndex,
        };
        var presentResult = _deviceApi.vkQueuePresentKHR(_graphicsQueue, &present);
        if (presentResult == VkResult.ErrorOutOfDateKHR || presentResult == VkResult.SuboptimalKHR)
        {
            _resizeRequested = true;
        }
        else if (presentResult != VkResult.Success)
        {
            presentResult.CheckResult();
        }

        _currentFrame = (_currentFrame + 1) % MaxFramesInFlight;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        if (_device != VkDevice.Null) _deviceApi.vkDeviceWaitIdle();

        for (int i = 0; i < MaxFramesInFlight; i++)
        {
            if (_inFlight[i] != VkFence.Null) _deviceApi.vkDestroyFence(_inFlight[i]);
            if (_imageAvailable[i] != VkSemaphore.Null)
                _deviceApi.vkDestroySemaphore(_imageAvailable[i]);
            if (_renderFinished[i] != VkSemaphore.Null)
                _deviceApi.vkDestroySemaphore(_renderFinished[i]);
        }

        if (_commandPool != VkCommandPool.Null)
            _deviceApi.vkDestroyCommandPool(_commandPool);
        DestroySwapchainObjects();
        if (_pipeline != VkPipeline.Null) _deviceApi.vkDestroyPipeline(_pipeline);
        if (_pipelineLayout != VkPipelineLayout.Null)
            _deviceApi.vkDestroyPipelineLayout(_pipelineLayout);
        if (_renderPass != VkRenderPass.Null) _deviceApi.vkDestroyRenderPass(_renderPass);

        if (_device != VkDevice.Null) _deviceApi.vkDestroyDevice();
        if (_surface != VkSurfaceKHR.Null) _instanceApi.vkDestroySurfaceKHR(_surface);
        if (_metalView != nint.Zero) SDL_Metal_DestroyView(_metalView);
        if (_instance != VkInstance.Null) _instanceApi.vkDestroyInstance();
    }

    private void CreateInstance()
    {
        uint sdlExtCount = 0;
        byte** sdlExtsPtr = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
        if (sdlExtsPtr == null)
        {
            throw new InvalidOperationException(
                $"SDL_Vulkan_GetInstanceExtensions failed: {SDL_GetErrorString()}");
        }

        var extensions = new List<VkUtf8String>((int)sdlExtCount + 2);
        for (uint i = 0; i < sdlExtCount; i++)
        {
            extensions.Add(new VkUtf8String(sdlExtsPtr[i]));
        }
        extensions.Add(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        var appName = (byte*)Marshal.StringToCoTaskMemUTF8("Glibre");
        try
        {
            var appInfo = new VkApplicationInfo
            {
                pApplicationName = appName,
                applicationVersion = new VkVersion(0, 1, 0),
                pEngineName = appName,
                engineVersion = new VkVersion(0, 1, 0),
                apiVersion = VK_API_VERSION_1_2,
            };

            using var extPin = new VkStringArray(extensions);
            var createInfo = new VkInstanceCreateInfo
            {
                flags = VkInstanceCreateFlags.EnumeratePortabilityKHR,
                pApplicationInfo = &appInfo,
                enabledExtensionCount = extPin.Length,
                ppEnabledExtensionNames = extPin,
            };
            vkCreateInstance(&createInfo, null, out _instance).CheckResult();
        }
        finally
        {
            Marshal.FreeCoTaskMem((nint)appName);
        }
    }

    private void CreateSurface()
    {
        // SDL_Vulkan_CreateSurface is not reliable when SDL and Vortice each dlopen
        // their own copy of the Vulkan loader. Take the CAMetalLayer from SDL and
        // create the surface through Vortice's own vkCreateMetalSurfaceEXT instead.
        _metalView = SDL_Metal_CreateView(_window);
        if (_metalView == nint.Zero)
        {
            throw new InvalidOperationException(
                $"SDL_Metal_CreateView failed: {SDL_GetErrorString()}");
        }
        nint layer = SDL_Metal_GetLayer(_metalView);
        if (layer == nint.Zero)
        {
            throw new InvalidOperationException(
                $"SDL_Metal_GetLayer failed: {SDL_GetErrorString()}");
        }

        var info = new VkMetalSurfaceCreateInfoEXT { pLayer = layer };
        _instanceApi.vkCreateMetalSurfaceEXT(&info, out _surface).CheckResult();
    }

    private void PickPhysicalDevice()
    {
        uint count = 0;
        _instanceApi.vkEnumeratePhysicalDevices(&count, null).CheckResult();
        if (count == 0)
        {
            throw new InvalidOperationException("No Vulkan-capable physical devices found.");
        }
        var devices = stackalloc VkPhysicalDevice[(int)count];
        _instanceApi.vkEnumeratePhysicalDevices(&count, devices).CheckResult();

        VkPhysicalDevice chosen = VkPhysicalDevice.Null;
        uint chosenQueueFamily = uint.MaxValue;
        var chosenType = VkPhysicalDeviceType.Other;

        for (int i = 0; i < count; i++)
        {
            var candidate = devices[i];
            if (!TryFindPresentQueueFamily(candidate, out uint qf)) continue;
            if (!SupportsSwapchain(candidate)) continue;

            _instanceApi.vkGetPhysicalDeviceProperties(candidate, out VkPhysicalDeviceProperties props);
            if (chosen == VkPhysicalDevice.Null
                || (chosenType != VkPhysicalDeviceType.DiscreteGpu
                    && props.deviceType == VkPhysicalDeviceType.DiscreteGpu))
            {
                chosen = candidate;
                chosenQueueFamily = qf;
                chosenType = props.deviceType;
            }
        }

        if (chosen == VkPhysicalDevice.Null)
        {
            throw new InvalidOperationException("No suitable Vulkan physical device.");
        }
        _physicalDevice = chosen;
        _graphicsQueueFamily = chosenQueueFamily;
    }

    private bool TryFindPresentQueueFamily(VkPhysicalDevice device, out uint family)
    {
        uint count = 0;
        _instanceApi.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, null);
        var families = stackalloc VkQueueFamilyProperties[(int)count];
        _instanceApi.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);

        for (uint i = 0; i < count; i++)
        {
            if ((families[i].queueFlags & VkQueueFlags.Graphics) == 0) continue;
            _instanceApi.vkGetPhysicalDeviceSurfaceSupportKHR(device, i, _surface, out VkBool32 supported)
                .CheckResult();
            if (supported)
            {
                family = i;
                return true;
            }
        }
        family = 0;
        return false;
    }

    private bool SupportsSwapchain(VkPhysicalDevice device)
        => DeviceHasExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    private bool DeviceHasExtension(VkPhysicalDevice device, ReadOnlySpan<byte> name)
    {
        uint count = 0;
        _instanceApi.vkEnumerateDeviceExtensionProperties(device, (byte*)null, &count, null)
            .CheckResult();
        var props = stackalloc VkExtensionProperties[(int)count];
        _instanceApi.vkEnumerateDeviceExtensionProperties(device, (byte*)null, &count, props)
            .CheckResult();
        for (uint i = 0; i < count; i++)
        {
            var extName = new ReadOnlySpan<byte>(props[i].extensionName, 256);
            int len = extName.IndexOf((byte)0);
            if (len < 0) len = extName.Length;
            if (extName[..len].SequenceEqual(name)) return true;
        }
        return false;
    }

    private void CreateDeviceAndQueue()
    {
        float priority = 1.0f;
        var queueInfo = new VkDeviceQueueCreateInfo
        {
            queueFamilyIndex = _graphicsQueueFamily,
            queueCount = 1,
            pQueuePriorities = &priority,
        };

        var extensions = new List<VkUtf8String>
        {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        if (DeviceHasExtension(_physicalDevice, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
        {
            extensions.Add(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }

        using var extPin = new VkStringArray(extensions);
        var deviceInfo = new VkDeviceCreateInfo
        {
            queueCreateInfoCount = 1,
            pQueueCreateInfos = &queueInfo,
            enabledExtensionCount = extPin.Length,
            ppEnabledExtensionNames = extPin,
        };
        _instanceApi.vkCreateDevice(_physicalDevice, &deviceInfo, null, out _device).CheckResult();
        // GetDeviceQueue is per-device; created VkDeviceApi after this method via outer constructor.
        // Temporary call through global vkGetDeviceProcAddr handled by VkDeviceApi later.
    }

    private void CreateSwapchain()
    {
        if (_graphicsQueue == VkQueue.Null)
        {
            _deviceApi.vkGetDeviceQueue(_graphicsQueueFamily, 0, out _graphicsQueue);
        }

        _instanceApi
            .vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, out var caps)
            .CheckResult();

        var format = PickSurfaceFormat();
        var presentMode = VkPresentModeKHR.Fifo;
        var extent = PickExtent(caps);

        uint imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        {
            imageCount = caps.maxImageCount;
        }

        var info = new VkSwapchainCreateInfoKHR
        {
            surface = _surface,
            minImageCount = imageCount,
            imageFormat = format.format,
            imageColorSpace = format.colorSpace,
            imageExtent = extent,
            imageArrayLayers = 1,
            imageUsage = VkImageUsageFlags.ColorAttachment,
            imageSharingMode = VkSharingMode.Exclusive,
            preTransform = caps.currentTransform,
            compositeAlpha = VkCompositeAlphaFlagsKHR.Opaque,
            presentMode = presentMode,
            clipped = true,
            oldSwapchain = VkSwapchainKHR.Null,
        };
        _deviceApi.vkCreateSwapchainKHR(&info, null, out _swapchain).CheckResult();

        _swapchainFormat = format.format;
        _swapchainExtent = extent;

        uint count = 0;
        _deviceApi.vkGetSwapchainImagesKHR(_swapchain, &count, null).CheckResult();
        _swapchainImages = new VkImage[count];
        fixed (VkImage* p = _swapchainImages)
        {
            _deviceApi.vkGetSwapchainImagesKHR(_swapchain, &count, p).CheckResult();
        }

        _swapchainImageViews = new VkImageView[count];
        for (int i = 0; i < count; i++)
        {
            var viewInfo = new VkImageViewCreateInfo
            {
                image = _swapchainImages[i],
                viewType = VkImageViewType.Image2D,
                format = _swapchainFormat,
                components = default,
                subresourceRange = new VkImageSubresourceRange
                {
                    aspectMask = VkImageAspectFlags.Color,
                    baseMipLevel = 0,
                    levelCount = 1,
                    baseArrayLayer = 0,
                    layerCount = 1,
                },
            };
            _deviceApi.vkCreateImageView(&viewInfo, null, out _swapchainImageViews[i])
                .CheckResult();
        }
    }

    private VkSurfaceFormatKHR PickSurfaceFormat()
    {
        uint count = 0;
        _instanceApi.vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &count, null)
            .CheckResult();
        var formats = stackalloc VkSurfaceFormatKHR[(int)count];
        _instanceApi.vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &count, formats)
            .CheckResult();
        for (uint i = 0; i < count; i++)
        {
            if (formats[i].format == VkFormat.B8G8R8A8Unorm
                && formats[i].colorSpace == VkColorSpaceKHR.SrgbNonLinear)
            {
                return formats[i];
            }
        }
        return formats[0];
    }

    private VkExtent2D PickExtent(VkSurfaceCapabilitiesKHR caps)
    {
        if (caps.currentExtent.width != uint.MaxValue)
        {
            return caps.currentExtent;
        }
        int pixelW, pixelH;
        SDL_GetWindowSizeInPixels(_window, &pixelW, &pixelH);
        return new VkExtent2D
        {
            width = Math.Clamp((uint)pixelW, caps.minImageExtent.width, caps.maxImageExtent.width),
            height = Math.Clamp((uint)pixelH, caps.minImageExtent.height, caps.maxImageExtent.height),
        };
    }

    private void CreateRenderPass()
    {
        var color = new VkAttachmentDescription
        {
            format = _swapchainFormat,
            samples = VkSampleCountFlags.Count1,
            loadOp = VkAttachmentLoadOp.Clear,
            storeOp = VkAttachmentStoreOp.Store,
            stencilLoadOp = VkAttachmentLoadOp.DontCare,
            stencilStoreOp = VkAttachmentStoreOp.DontCare,
            initialLayout = VkImageLayout.Undefined,
            finalLayout = VkImageLayout.PresentSrcKHR,
        };
        var colorRef = new VkAttachmentReference
        {
            attachment = 0,
            layout = VkImageLayout.ColorAttachmentOptimal,
        };
        var subpass = new VkSubpassDescription
        {
            pipelineBindPoint = VkPipelineBindPoint.Graphics,
            colorAttachmentCount = 1,
            pColorAttachments = &colorRef,
        };
        var dependency = new VkSubpassDependency
        {
            srcSubpass = VK_SUBPASS_EXTERNAL,
            dstSubpass = 0,
            srcStageMask = VkPipelineStageFlags.ColorAttachmentOutput,
            dstStageMask = VkPipelineStageFlags.ColorAttachmentOutput,
            srcAccessMask = 0,
            dstAccessMask = VkAccessFlags.ColorAttachmentWrite,
        };
        var info = new VkRenderPassCreateInfo
        {
            attachmentCount = 1,
            pAttachments = &color,
            subpassCount = 1,
            pSubpasses = &subpass,
            dependencyCount = 1,
            pDependencies = &dependency,
        };
        _deviceApi.vkCreateRenderPass(&info, null, out _renderPass).CheckResult();
    }

    private void CreatePipeline()
    {
        var vertSpv = CompileGlsl(VertexShaderSource, ShaderKind.VertexShader, "triangle.vert");
        var fragSpv = CompileGlsl(FragmentShaderSource, ShaderKind.FragmentShader, "triangle.frag");

        VkShaderModule vert, frag;
        fixed (byte* p = vertSpv)
        {
            var ci = new VkShaderModuleCreateInfo
            {
                codeSize = (nuint)vertSpv.Length,
                pCode = (uint*)p,
            };
            _deviceApi.vkCreateShaderModule(&ci, null, out vert).CheckResult();
        }
        fixed (byte* p = fragSpv)
        {
            var ci = new VkShaderModuleCreateInfo
            {
                codeSize = (nuint)fragSpv.Length,
                pCode = (uint*)p,
            };
            _deviceApi.vkCreateShaderModule(&ci, null, out frag).CheckResult();
        }

        try
        {
            var entry = (byte*)Marshal.StringToCoTaskMemUTF8("main");
            try
            {
                var stages = stackalloc VkPipelineShaderStageCreateInfo[2];
                stages[0] = new VkPipelineShaderStageCreateInfo
                {
                    stage = VkShaderStageFlags.Vertex,
                    module = vert,
                    pName = entry,
                };
                stages[1] = new VkPipelineShaderStageCreateInfo
                {
                    stage = VkShaderStageFlags.Fragment,
                    module = frag,
                    pName = entry,
                };

                var vertexInput = new VkPipelineVertexInputStateCreateInfo();
                var inputAssembly = new VkPipelineInputAssemblyStateCreateInfo
                {
                    topology = VkPrimitiveTopology.TriangleList,
                };
                var viewport = new VkPipelineViewportStateCreateInfo
                {
                    viewportCount = 1,
                    scissorCount = 1,
                };
                var rasterizer = new VkPipelineRasterizationStateCreateInfo
                {
                    polygonMode = VkPolygonMode.Fill,
                    cullMode = VkCullModeFlags.None,
                    frontFace = VkFrontFace.CounterClockwise,
                    lineWidth = 1.0f,
                };
                var multisample = new VkPipelineMultisampleStateCreateInfo
                {
                    rasterizationSamples = VkSampleCountFlags.Count1,
                };
                var blendAttachment = new VkPipelineColorBlendAttachmentState
                {
                    blendEnable = false,
                    colorWriteMask = VkColorComponentFlags.R | VkColorComponentFlags.G
                                     | VkColorComponentFlags.B | VkColorComponentFlags.A,
                };
                var blend = new VkPipelineColorBlendStateCreateInfo
                {
                    attachmentCount = 1,
                    pAttachments = &blendAttachment,
                };

                var dynStates = stackalloc VkDynamicState[2];
                dynStates[0] = VkDynamicState.Viewport;
                dynStates[1] = VkDynamicState.Scissor;
                var dyn = new VkPipelineDynamicStateCreateInfo
                {
                    dynamicStateCount = 2,
                    pDynamicStates = dynStates,
                };

                var layoutInfo = new VkPipelineLayoutCreateInfo();
                _deviceApi.vkCreatePipelineLayout(&layoutInfo, null, out _pipelineLayout)
                    .CheckResult();

                var pipelineInfo = new VkGraphicsPipelineCreateInfo
                {
                    stageCount = 2,
                    pStages = stages,
                    pVertexInputState = &vertexInput,
                    pInputAssemblyState = &inputAssembly,
                    pViewportState = &viewport,
                    pRasterizationState = &rasterizer,
                    pMultisampleState = &multisample,
                    pColorBlendState = &blend,
                    pDynamicState = &dyn,
                    layout = _pipelineLayout,
                    renderPass = _renderPass,
                    subpass = 0,
                };
                fixed (VkPipeline* pPipeline = &_pipeline)
                {
                    _deviceApi
                        .vkCreateGraphicsPipelines(VkPipelineCache.Null, 1, &pipelineInfo, null, pPipeline)
                        .CheckResult();
                }
            }
            finally
            {
                Marshal.FreeCoTaskMem((nint)entry);
            }
        }
        finally
        {
            _deviceApi.vkDestroyShaderModule(vert);
            _deviceApi.vkDestroyShaderModule(frag);
        }
    }

    private static byte[] CompileGlsl(string source, ShaderKind kind, string name)
    {
        using var compiler = new Compiler();
        var options = new CompilerOptions
        {
            ShaderStage = kind,
            SourceLanguage = SourceLanguage.GLSL,
            TargetEnv = TargetEnvironmentVersion.Vulkan_1_2,
            OptimizationLevel = OptimizationLevel.Performance,
        };
        var result = compiler.Compile(source, name, options);
        if (result.Status != CompilationStatus.Success)
        {
            throw new InvalidOperationException(
                $"shaderc failed to compile {name}: {result.ErrorMessage}");
        }
        return result.Bytecode;
    }

    private void CreateFramebuffers()
    {
        _framebuffers = new VkFramebuffer[_swapchainImageViews.Length];
        for (int i = 0; i < _swapchainImageViews.Length; i++)
        {
            var attachment = _swapchainImageViews[i];
            var info = new VkFramebufferCreateInfo
            {
                renderPass = _renderPass,
                attachmentCount = 1,
                pAttachments = &attachment,
                width = _swapchainExtent.width,
                height = _swapchainExtent.height,
                layers = 1,
            };
            _deviceApi.vkCreateFramebuffer(&info, null, out _framebuffers[i]).CheckResult();
        }
    }

    private void CreateCommandPool()
    {
        var info = new VkCommandPoolCreateInfo
        {
            flags = VkCommandPoolCreateFlags.ResetCommandBuffer,
            queueFamilyIndex = _graphicsQueueFamily,
        };
        _deviceApi.vkCreateCommandPool(&info, null, out _commandPool).CheckResult();

        var alloc = new VkCommandBufferAllocateInfo
        {
            commandPool = _commandPool,
            level = VkCommandBufferLevel.Primary,
            commandBufferCount = MaxFramesInFlight,
        };
        fixed (VkCommandBuffer* p = _commandBuffers)
        {
            _deviceApi.vkAllocateCommandBuffers(&alloc, p).CheckResult();
        }
    }

    private void CreateSyncObjects()
    {
        var semInfo = new VkSemaphoreCreateInfo();
        var fenceInfo = new VkFenceCreateInfo { flags = VkFenceCreateFlags.Signaled };
        for (int i = 0; i < MaxFramesInFlight; i++)
        {
            _deviceApi.vkCreateSemaphore(&semInfo, null, out _imageAvailable[i]).CheckResult();
            _deviceApi.vkCreateSemaphore(&semInfo, null, out _renderFinished[i]).CheckResult();
            _deviceApi.vkCreateFence(&fenceInfo, null, out _inFlight[i]).CheckResult();
        }
    }

    private void RecordCommandBuffer(VkCommandBuffer cmd, uint imageIndex)
    {
        var begin = new VkCommandBufferBeginInfo();
        _deviceApi.vkBeginCommandBuffer(cmd, &begin).CheckResult();

        var clear = new VkClearValue { color = new VkClearColorValue(0.08f, 0.08f, 0.10f, 1.0f) };
        var pass = new VkRenderPassBeginInfo
        {
            renderPass = _renderPass,
            framebuffer = _framebuffers[imageIndex],
            renderArea = new VkRect2D(0, 0, _swapchainExtent.width, _swapchainExtent.height),
            clearValueCount = 1,
            pClearValues = &clear,
        };
        _deviceApi.vkCmdBeginRenderPass(cmd, &pass, VkSubpassContents.Inline);

        var viewport = new VkViewport
        {
            x = 0,
            y = 0,
            width = _swapchainExtent.width,
            height = _swapchainExtent.height,
            minDepth = 0,
            maxDepth = 1,
        };
        _deviceApi.vkCmdSetViewport(cmd, 0, 1, &viewport);
        var scissor = new VkRect2D(0, 0, _swapchainExtent.width, _swapchainExtent.height);
        _deviceApi.vkCmdSetScissor(cmd, 0, 1, &scissor);

        _deviceApi.vkCmdBindPipeline(cmd, VkPipelineBindPoint.Graphics, _pipeline);
        _deviceApi.vkCmdDraw(cmd, 3, 1, 0, 0);

        _deviceApi.vkCmdEndRenderPass(cmd);
        _deviceApi.vkEndCommandBuffer(cmd).CheckResult();
    }

    private void RecreateSwapchain()
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(_window, &w, &h);
        while (w == 0 || h == 0)
        {
            SDL_WaitEvent(null);
            SDL_GetWindowSizeInPixels(_window, &w, &h);
        }
        _deviceApi.vkDeviceWaitIdle();
        DestroySwapchainObjects();
        CreateSwapchain();
        CreateFramebuffers();
    }

    private void DestroySwapchainObjects()
    {
        for (int i = 0; i < _framebuffers.Length; i++)
        {
            if (_framebuffers[i] != VkFramebuffer.Null)
            {
                _deviceApi.vkDestroyFramebuffer(_framebuffers[i]);
            }
        }
        _framebuffers = [];
        for (int i = 0; i < _swapchainImageViews.Length; i++)
        {
            if (_swapchainImageViews[i] != VkImageView.Null)
            {
                _deviceApi.vkDestroyImageView(_swapchainImageViews[i]);
            }
        }
        _swapchainImageViews = [];
        if (_swapchain != VkSwapchainKHR.Null)
        {
            _deviceApi.vkDestroySwapchainKHR(_swapchain);
            _swapchain = VkSwapchainKHR.Null;
        }
    }

    private static string SDL_GetErrorString() => SDL_GetError() ?? "";
}
