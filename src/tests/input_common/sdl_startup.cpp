// SPDX-License-Identifier: GPL-2.0-or-later
#include <memory>
#include <SDL.h>
#include <catch2/catch_test_macros.hpp>
#include "common/logging/backend.h"
#include "input_common/sdl/sdl_impl.h"

namespace {
struct ControllerSession {
    ~ControllerSession() {
        state.reset();
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    std::unique_ptr<InputCommon::SDL::SDLState> state;
};
} // namespace

TEST_CASE("SDL enumerates existing controllers and handles reconnects", "[input][sdl]") {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    Common::Log::DisableLoggingInTests();
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0);
    ControllerSession session;
    const int initial_count = SDL_NumJoysticks();
    const int first = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1);
    REQUIRE(first >= 0);
    REQUIRE(SDL_IsGameController(first) == SDL_TRUE);
    const auto first_id = SDL_JoystickGetDeviceInstanceID(first);

    session.state = std::make_unique<InputCommon::SDL::SDLState>();
    const auto initial = session.state->ConnectedControllers().size();
    REQUIRE(initial >= 1);
    REQUIRE(session.state->GetSDLJoystickBySDLID(first_id) != nullptr);

    const int second = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1);
    REQUIRE(second >= 0);
    SDL_PumpEvents();
    REQUIRE(session.state->ConnectedControllers().size() == initial + 1);
    REQUIRE(SDL_JoystickDetachVirtual(second) == 0);
    SDL_PumpEvents();
    REQUIRE(session.state->ConnectedControllers().size() == initial);
    REQUIRE(SDL_JoystickDetachVirtual(first) == 0);
    SDL_PumpEvents();
    REQUIRE(session.state->ConnectedControllers().size() == initial - 1);

    const int reconnected = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1);
    REQUIRE(reconnected >= 0);
    SDL_PumpEvents();
    REQUIRE(session.state->ConnectedControllers().size() == initial);
    REQUIRE(session.state->GetSDLJoystickBySDLID(SDL_JoystickGetDeviceInstanceID(reconnected)) !=
            nullptr);
    REQUIRE(SDL_JoystickDetachVirtual(reconnected) == 0);
    SDL_PumpEvents();
    REQUIRE(SDL_NumJoysticks() == initial_count);
#else
    SKIP("Virtual joysticks require SDL 2.0.14 or newer");
#endif
}
