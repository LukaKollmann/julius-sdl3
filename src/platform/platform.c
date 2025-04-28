#include "platform.h"

#include <SDL3/SDL.h>

int platform_sdl_version_at_least(int major, int minor, int patch)
{
    int v = SDL_GetVersion();
    return v >= SDL_VERSIONNUM(major, minor, patch);
}
