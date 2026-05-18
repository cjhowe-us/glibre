using GLI.Runtime;
using SDL;
using static SDL.SDL3;

unsafe
{
    if (!SDL_Init(SDL_InitFlags.SDL_INIT_VIDEO))
    {
        throw new InvalidOperationException($"SDL_Init failed: {SDL_GetError()}");
    }
    try
    {
        if (!SDL_Vulkan_LoadLibrary((byte*)null))
        {
            throw new InvalidOperationException(
                $"SDL_Vulkan_LoadLibrary failed: {SDL_GetError()}");
        }

        var flags = SDL_WindowFlags.SDL_WINDOW_VULKAN
                  | SDL_WindowFlags.SDL_WINDOW_RESIZABLE
                  | SDL_WindowFlags.SDL_WINDOW_HIGH_PIXEL_DENSITY;
        SDL_Window* window = SDL_CreateWindow("Glibre", 1440, 960, flags);
        if (window == null)
        {
            throw new InvalidOperationException($"SDL_CreateWindow failed: {SDL_GetError()}");
        }

        try
        {
            using var renderer = new TriangleRenderer(window);

            bool running = true;
            while (running)
            {
                SDL_Event evt;
                while (SDL_PollEvent(&evt))
                {
                    switch ((SDL_EventType)evt.type)
                    {
                        case SDL_EventType.SDL_EVENT_QUIT:
                        case SDL_EventType.SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                            running = false;
                            break;
                        case SDL_EventType.SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        case SDL_EventType.SDL_EVENT_WINDOW_RESIZED:
                            renderer.OnResize();
                            break;
                    }
                }
                if (!running) break;
                renderer.DrawFrame();
            }
        }
        finally
        {
            SDL_DestroyWindow(window);
            SDL_Vulkan_UnloadLibrary();
        }
    }
    finally
    {
        SDL_Quit();
    }
}
