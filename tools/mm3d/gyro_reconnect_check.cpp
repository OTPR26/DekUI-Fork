// SPDX-License-Identifier: GPL-2.0-or-later
// Fake SDL handles isolate handle-scoped sensor enablement from real hardware.
#include <SDL.h>
#include <cassert>
#include "input_common/sdl/sdl_joystick.h"
static unsigned enabled_sensors=0;
extern "C" {
void SDL_JoystickClose(SDL_Joystick*) {}
void SDL_GameControllerClose(SDL_GameController*) {}
int SDL_JoystickNumAxes(SDL_Joystick*) {return 0;}
Sint16 SDL_JoystickGetAxis(SDL_Joystick*,int) {return 0;}
Uint8 SDL_JoystickGetButton(SDL_Joystick*,int) {return 0;}
Uint8 SDL_JoystickGetHat(SDL_Joystick*,int) {return 0;}
Sint16 SDL_GameControllerGetAxis(SDL_GameController*,SDL_GameControllerAxis) {return 0;}
Uint8 SDL_GameControllerGetButton(SDL_GameController*,SDL_GameControllerButton) {return 0;}
SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController*,SDL_GameControllerButton) {return {};}
SDL_GameControllerButtonBind SDL_GameControllerGetBindForAxis(SDL_GameController*,SDL_GameControllerAxis) {return {};}
SDL_bool SDL_GameControllerHasSensor(SDL_GameController*,SDL_SensorType) {return SDL_TRUE;}
int SDL_GameControllerSetSensorEnabled(SDL_GameController*,SDL_SensorType,SDL_bool enabled) {if(enabled)++enabled_sensors;return 0;}
}
int main() {
    using InputCommon::SDL::SDLJoystick;
    auto* joystick=reinterpret_cast<SDL_Joystick*>(1);
    auto* controller=reinterpret_cast<SDL_GameController*>(2);
    SDLJoystick device("test",0,joystick,controller);
    assert(enabled_sensors==2&&device.HasMotion());
    for(unsigned reconnect=0;reconnect<3;++reconnect) {
        device.SetAccel(1,2,3);device.SetGyro(4,5,6);device.SetTouchpad(.4f,.6f,0,true);
        device.SetSDLJoystick(nullptr,nullptr);
        const auto [accel,gyro]=device.GetMotion();
        assert(accel.x==0&&accel.y==0&&accel.z==0&&gyro.x==0&&gyro.y==0&&gyro.z==0);
        assert(!device.HasMotion()&&!std::get<2>(device.GetTouch(0)));
        device.SetSDLJoystick(joystick,controller);
        assert(enabled_sensors==2*(reconnect+2)&&device.HasMotion());
        device.SetGyro(7,8,9);assert(std::get<1>(device.GetMotion()).y==8);
    }
    SDLJoystick pending("test",0,nullptr,nullptr);
    pending.SetSDLJoystick(joystick,controller);
    assert(pending.HasMotion()&&enabled_sensors==10);
}
