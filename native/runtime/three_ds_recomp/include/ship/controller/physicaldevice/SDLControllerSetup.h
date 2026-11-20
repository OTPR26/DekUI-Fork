#pragma once

#include <SDL2/SDL.h>

namespace Ship {

// Azahar input_common/sdl/sdl_impl.cpp enables extended Sony reports to expose
// motion. Apply before opening devices, but respect user/Steam environment
// overrides. Do not create a competing input backend or force virtual pads off.
inline void ConfigureSDLControllerCapabilities() {
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI, "1", SDL_HINT_DEFAULT);
    // Steam Input exposes a pad to Linux/SteamOS sessions as its own uinput
    // virtual gamepad, and SDL discards that device unless the application opts
    // in. While Steam owns the physical pad it is not enumerable, so without
    // this a Steam Deck session has no controller at all. This must be the
    // process environment, not a hint: SDL3 - and the SDL2-compat library the
    // AppImage ships - tests the variable directly, and SDL2 reads the
    // environment before its hint table. overwrite=0 keeps an explicit Steam
    // or user value authoritative, and SDL2 2.32 publishes no macro for it.
    SDL_setenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1", SDL_HINT_DEFAULT);
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1", SDL_HINT_DEFAULT);
#endif
#ifdef SDL_HINT_ACCELEROMETER_AS_JOYSTICK
    SDL_SetHintWithPriority(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0", SDL_HINT_DEFAULT);
#endif
}

} // namespace Ship
