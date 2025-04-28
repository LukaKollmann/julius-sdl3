#include "core/calc.h"
#include "game/system.h"
#include "input/mouse.h"
#include "platform/screen.h"

#include <SDL3/SDL_mouse.h>

static struct {
    int x;
    int y;
    int enabled;
} data;

void system_mouse_get_relative_state(int *x, int *y)
{
    if (!x || !y) { return; }
    float fx, fy;
    SDL_GetRelativeMouseState(&fx, &fy);
    *x = (int) fx;
    *y = (int) fy;
}

void system_mouse_set_relative_mode(int enabled)
{
    if (enabled == data.enabled) {
        return;
    }
    SDL_Window *window = SDL_GetMouseFocus();
    if (enabled) {
        float fx, fy;
        SDL_GetMouseState(&fx, &fy);
        data.x = (int) fx;
        data.y = (int) fy;
        int scale_percentage = calc_percentage(100, platform_screen_get_scale());
        data.x = calc_adjust_with_percentage(data.x, scale_percentage);
        data.y = calc_adjust_with_percentage(data.y, scale_percentage);
        SDL_SetWindowRelativeMouseMode(window,true);
        // Discard the first value, which is incorrect
        // (the first one gives the relative position to center of window)
        system_mouse_get_relative_state(NULL, NULL);
    } else {
        SDL_SetWindowRelativeMouseMode(window,false);
        system_set_mouse_position(&data.x, &data.y);
        mouse_set_position(data.x, data.y);
    }
    data.enabled = enabled;
}

void system_move_mouse_cursor(int delta_x, int delta_y)
{
    int x = mouse_get()->x + delta_x;
    int y = mouse_get()->y + delta_y;
    system_set_mouse_position(&x, &y);
    mouse_set_position(x, y);
}
