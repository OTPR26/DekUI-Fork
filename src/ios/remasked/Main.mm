#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#import <CoreMotion/CoreMotion.h>
#import <AVFoundation/AVFoundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "RetroVideo.h"
#include "drive_data.h"
#include "AzaharTouchHost.h"
#include "RemaskedTouchBridge.h"
#include "RemaskedSettingsBridge.h"
#include "RemaskedController.h"
#import "SettingsPanel.h"
#include "mm3d/touch_hud.h"
#include "mm3d/simple_cheats.h"
#include "touch_controls.h"
#include "portrait_layout.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include <cmath>
#include <cstring>
#include <memory>
#include <os/log.h>
#import <objc/message.h>

static RetroVideo video;
static retro_hw_render_callback hardware{};
static retro_hw_render_context_negotiation_interface_vulkan negotiation{};
static std::map<std::string, std::string> options;
static std::string dataDirectory;
static RemaskedGameSettings gameSettings;
static bool optionsChanged;
static bool touchControlsEnabled = true;
static bool touchVibration;
static bool landscapeArtwork = true;
static MTKView* airPlayScreen;
static bool airPlayDual;
enum class AirPlayMode { SingleScreen, TopScreen };
static NSInteger motionSource = 1;
static bool invertMotionX, invertMotionY;
namespace Controller = ReMaskedController;
static Controller::Assignments controllerAssignments;
static int movementDeadZone, cameraDeadZone, triggerDeadZone;
static float gyroSensitivity = 1.f;
constexpr std::array PictureAspects{RemaskedNativeAspect, 16.f / 9.f, 21.f / 9.f};

static NSString* PreferenceKey(const char* key) {
    return [@"ReMasked.Azahar." stringByAppendingString:@(key)];
}

static int Preference(const char* key, int fallback) {
    NSNumber* value = [NSUserDefaults.standardUserDefaults objectForKey:PreferenceKey(key)];
    return value ? value.intValue : fallback;
}

static void SavePreference(const char* key, int value) {
    [NSUserDefaults.standardUserDefaults setInteger:value forKey:PreferenceKey(key)];
}

static std::string ControllerPreference(const Controller::Binding& binding) {
    return "Controller." + std::string(binding.key);
}

static std::string HudPreference(unsigned part, const char* property) {
    return "Hud." + std::string(MM3D::HudAdjustments::Keys[part]) + "." + property;
}

static void LoadGamePreferences() {
    for (unsigned pose = 0; pose < gameSettings.title.size(); ++pose)
        for (unsigned axis = 0; axis < gameSettings.title[pose].size(); ++axis) {
            const auto key = "Title." + std::to_string(pose) + "." + std::to_string(axis);
            gameSettings.title[pose][axis] =
                Preference(key.c_str(), std::lround(RemaskedGameSettings{}.title[pose][axis] * 100)) / 100.f;
        }
    gameSettings.hudSize = Preference("HudSize", 0);
    gameSettings.hudOpacity = Preference("HudOpacity", 100);
    for (unsigned part = 0; part < gameSettings.hudParts.size(); ++part) {
        gameSettings.hudParts[part] = {Preference(HudPreference(part, "Size").c_str(), 0),
            Preference(HudPreference(part, "X").c_str(), 0),
            Preference(HudPreference(part, "Y").c_str(), 0)};
    }
    gameSettings.magicHeight = Preference("HudMagicHeight", 0);
    gameSettings.heartSpacing = Preference("HudHeartSpacing", 0);
    gameSettings.hideMenuPrompts = Preference("HideItemMenuPrompts", 0);
    gameSettings.singleScreenInstructions = Preference("SingleScreenInstructions", 1);
    gameSettings.controllerNeutralItemPrompts = Preference("ControllerNeutralItemPrompts", 1);
    gameSettings.disableTatlReminders = Preference("TatlRemindersOff", 0);
    gameSettings.muteMusic = Preference("MuteMusic", 0);
    gameSettings.disableLowHealthBeep = Preference("LowHealthBeepOff", 0);
    auto& gameplay = gameSettings.gameplay;
    gameplay.disableBomberApproach = Preference("BomberApproachOff", 0);
    gameplay.swordActivateOwls = Preference("SwordActivateOwls", 0);
    gameplay.classicDoubleTime = Preference("ClassicDoubleTime", 0);
    gameplay.fastTime = Preference("FastTime", 0);
    gameplay.damageMultiplier = Preference("DamageMultiplier", 1);
    gameplay.weakerEnhancedDefense = Preference("WeakerEnhancedDefense", 0);
    gameplay.disableHeartDrops = Preference("HeartDropsOff", 0);
    gameplay.disableRupeeDrops = Preference("RupeeDropsOff", 0);
    gameplay.disableFairyDrops = Preference("FairyDropsOff", 0);
    gameplay.disableMagicDrops = Preference("MagicDropsOff", 0);
    gameplay.disableAmmoPickups = Preference("AmmoPickupsOff", 0);
    gameplay.loseRupeesOnDeath = Preference("LoseRupeesOnDeath", 0);
    gameplay.endWorldOnDeath = Preference("EndWorldOnDeath", 0);
    gameplay.advanceTimeOnDeath = Preference("AdvanceTimeOnDeath", 0);
    gameplay.doubleShopPrices = Preference("DoubleShopPrices", 0);
    gameplay.classicSaving = Preference("ClassicSaving", 0);
    gameplay.redPotionHearts = Preference("RedPotionHearts", 20);
    gameplay.bluePotionHearts = Preference("BluePotionHearts", 20);
    gameplay.bottledFairyHearts = Preference("BottledFairyHearts", 10);
    constexpr const char* speedKeys[]{"WalkingSpeed", "ClimbingSpeed", "SwimmingSpeed"};
    for (unsigned i = 0; i < gameSettings.speed.size(); ++i)
        gameSettings.speed[i] = Preference(speedKeys[i], 100) / 100.f;
    gameSettings.cameraDistance = Preference("CameraDistance", 100) / 100.f;
    touchControlsEnabled = Preference("TouchVisible", 1);
    touchVibration = Preference("TouchVibration", 0);
    landscapeArtwork = Preference("LandscapeArtwork", 1);
    motionSource = Preference("MotionSource", 1);
    invertMotionX = Preference("InvertMotionX", 0);
    invertMotionY = Preference("InvertMotionY", 0);
    for (unsigned i = 0; i < controllerAssignments.size(); ++i) {
        const auto& binding = Controller::Bindings[i];
        controllerAssignments[i] = static_cast<Controller::Button>(
            std::clamp(Preference(ControllerPreference(binding).c_str(), binding.defaultButton),
                       0, int(Controller::Count) - 1));
    }
    movementDeadZone = std::clamp(Preference("MovementStickDeadZone", 0), 0, 50);
    cameraDeadZone = std::clamp(Preference("CameraStickDeadZone", 0), 0, 50);
    triggerDeadZone = std::clamp(Preference("TriggerDeadZone", 0), 0, 50);
    gyroSensitivity = std::clamp(Preference("GyroSensitivity", 100), 25, 400) / 100.f;
}

constexpr int MaxInternalResolutionScale = 6;
static constexpr std::array GameLanguages{"English", "Japanese", "French", "Spanish", "German",
    "Italian", "Dutch", "Portuguese", "Russian", "Korean", "Traditional Chinese", "Simplified Chinese"};

static void UpdateGraphicsOptions() {
    const auto toggle = [](const char* key) { return Preference(key, 1) ? "enabled" : "disabled"; };
    const std::array filters{"none", "Bicubic", "ScaleForce", "xBRZ", "MMPX"};
    options["citra_language_value"] = GameLanguages[std::clamp(Preference("GameLanguage", 0),
        0, int(GameLanguages.size()) - 1)];
    options["citra_resolution_factor"] = std::to_string(std::clamp(Preference("Resolution", 0), 0, MaxInternalResolutionScale - 1) + 1);
    options["citra_texture_filter"] = filters[std::clamp(Preference("TextureFilter", 0), 0, 4)];
    options["citra_custom_textures"] = toggle("CustomTextures");
    options["citra_use_disk_shader_cache"] = toggle("ShaderCache");
    optionsChanged = true;
}

static std::atomic<uint16_t> touchButtons;
static MM3D::TouchHud::State touchHud;
static CGRect touchPicture = CGRectZero;
static CGRect bottomPicture = CGRectNull;
static int16_t bottomTouchX, bottomTouchY;
static bool bottomTouchPressed;
static RemaskedPictureSettings pictureSettings;
static std::atomic<int16_t> moveX, moveY, lookX, lookY;
static std::atomic<bool> touchPictograph;
static bool pictographStickHeld;
static dispatch_block_t touchSettings;
static CMMotionManager* motion;
static std::atomic<int> motionOrientation;

static std::array<std::array<double, 3>, 2> motionCalibration;

static NSString* motionCalibrationKey(bool external) {
    return PreferenceKey(external ? "ExternalMotionBias" : "DeviceMotionBias");
}

static void loadMotionCalibration() {
    for (unsigned source = 0; source < motionCalibration.size(); ++source) {
        NSArray<NSNumber*>* values = [NSUserDefaults.standardUserDefaults arrayForKey:motionCalibrationKey(source == 1)];
        if (values.count != 3) continue;
        for (unsigned axis = 0; axis < 3; ++axis)
            motionCalibration[source][axis] = values[axis].doubleValue;
    }
}

static void startDeviceMotion() {
    if (!motion) {
        motion = [CMMotionManager new];
        motion.deviceMotionUpdateInterval = .01;
    }
    if (motion.deviceMotionAvailable && !motion.deviceMotionActive) [motion startDeviceMotionUpdates];
}

static GCMotion* controllerMotion(GCController* controller) {
    GCMotion* sensors = controller.motion;
    if (!sensors.hasRotationRate) return nil;
    if (sensors.sensorsRequireManualActivation && !sensors.sensorsActive) sensors.sensorsActive = YES;
    return sensors;
}
static std::string coreMessage;
static AVAudioEngine* audioEngine;
static AVAudioPlayerNode* audioPlayer;
static AVAudioFormat* audioFormat;

static int16_t axis(float value) { return int16_t(std::clamp(value,-1.f,1.f)*32767); }

static GCController* gameplayController() {
    if (GCController.current.extendedGamepad) return GCController.current;
    for (GCController* controller in GCController.controllers)
        if (controller.extendedGamepad) return controller;
    return nil;
}

namespace Menu = Oot3dNativeGame::Menu;

static std::optional<Menu::Input> menuControllerInput() {
    GCExtendedGamepad* pad = gameplayController().extendedGamepad;
    if (!pad) return {};
    const float x = pad.leftThumbstick.xAxis.value, y = pad.leftThumbstick.yAxis.value;
    const bool vertical = std::abs(y) >= std::abs(x);
    return Menu::Input{
        .Up = bool(pad.dpad.up.pressed) || (vertical && y > .55f),
        .Down = bool(pad.dpad.down.pressed) || (vertical && y < -.55f),
        .Left = bool(pad.dpad.left.pressed) || (!vertical && x < -.55f),
        .Right = bool(pad.dpad.right.pressed) || (!vertical && x > .55f),
        .Confirm = bool(pad.buttonA.pressed),
        .Back = bool(pad.buttonB.pressed),
        .TabPrevious = bool(pad.leftShoulder.pressed),
        .TabNext = bool(pad.rightShoulder.pressed)};
}

static Menu::Row PreferenceRow(std::string key, std::string label, int fallback, int minimum,
                               int maximum, std::function<void()> apply = {},
                               std::function<std::string(int)> format = {}) {
    Menu::Row row;
    row.Kind = Menu::RowKind::Number;
    row.Key = std::move(label);
    row.Minimum = minimum;
    row.Maximum = maximum;
    row.Format = std::move(format);
    row.Read = [=] { return std::clamp(Preference(key.c_str(), fallback), minimum, maximum); };
    row.Write = [=](int value) {
        SavePreference(key.c_str(), std::clamp(value, minimum, maximum));
        if (apply) apply();
        return Menu::WriteResult{};
    };
    return row;
}

static Menu::Row PreferenceToggle(std::string key, std::string label, bool initial,
                                  std::function<void()> apply = {}) {
    auto row = PreferenceRow(std::move(key), std::move(label), initial, 0, 1, std::move(apply));
    row.Kind = Menu::RowKind::Toggle;
    return row;
}

static bool nintendoController(GCController* controller) {
    NSString* category = controller.productCategory;
    return [category isEqualToString:@"Switch Pro Controller"] ||
        [category isEqualToString:@"Nintendo Switch Pro Controller"];
}

static Controller::PhysicalButtons physicalControllerButtons(GCExtendedGamepad* pad, bool switchPro) {
    if (!pad) return 0;
    using namespace ReMaskedController;
    PhysicalButtons buttons = 0;
    auto add = [&](bool pressed, Button button) {
        if (pressed) buttons |= PhysicalBit(button);
    };
    // Apple presents Switch Pro buttons by their printed labels.
    add(switchPro ? pad.buttonA.pressed : pad.buttonB.pressed, East);
    add(switchPro ? pad.buttonB.pressed : pad.buttonA.pressed, South);
    add(switchPro ? pad.buttonX.pressed : pad.buttonY.pressed, North);
    add(switchPro ? pad.buttonY.pressed : pad.buttonX.pressed, West);
    add(pad.leftShoulder.pressed, LeftBumper);
    add(pad.rightShoulder.pressed, RightBumper);
    add(Trigger(pad.leftTrigger.pressed, pad.leftTrigger.value, triggerDeadZone), LeftTrigger);
    add(Trigger(pad.rightTrigger.pressed, pad.rightTrigger.value, triggerDeadZone), RightTrigger);
    add(pad.buttonMenu.pressed, MenuButton);
    add(pad.buttonOptions.pressed, OptionsButton);
    add(pad.dpad.up.pressed, DpadUp);
    add(pad.dpad.down.pressed, DpadDown);
    add(pad.dpad.left.pressed, DpadLeft);
    add(pad.dpad.right.pressed, DpadRight);
    add(pad.leftThumbstickButton.pressed, LeftStickClick);
    add(pad.rightThumbstickButton.pressed, RightStickClick);
    add(pad.buttonHome.pressed, GuideButton);
    return buttons;
}

static std::atomic<uint16_t> gamepadButtons;
static bool settingsBackHeldOnResume;
static bool launcherConfirmHeldOnStart;
static std::array<std::atomic<int16_t>, 4> gamepadAxes{};
static std::atomic<bool> gamepadPictograph;
static std::array<std::atomic<float>, 6> motionValues{};

static std::array<float, 6> sampleMotion(GCController* controller, bool switchPro) {
    if (motionSource == 0) return {};
    const double ix = invertMotionX ? -1 : 1, iy = invertMotionY ? -1 : 1;
    const auto sample = [iy](auto gyro, auto gravity, auto acceleration,
                             UIInterfaceOrientation orientation, double aimX) -> std::array<float, 6> {
        const auto orient = [orientation](double x, double y, double z) {
            switch (orientation) {
            case UIInterfaceOrientationLandscapeLeft: return std::array{y, -x, z};
            case UIInterfaceOrientationLandscapeRight: return std::array{-y, x, z};
            case UIInterfaceOrientationPortraitUpsideDown: return std::array{-x, -y, z};
            default: return std::array{x, y, z};
            }
        };
        const auto a = orient(gravity.x + acceleration.x, gravity.y + acceleration.y,
                              gravity.z + acceleration.z);
        const auto r = orient(gyro.x, gyro.y, gyro.z);
        // Libretro expects acceleration in g and performs the radians conversion.
        return {float(a[0] * aimX), float(-a[1]), float(a[2] * iy),
                float(-r[0] * iy * gyroSensitivity), float(r[1] * aimX * gyroSensitivity),
                float(-r[2] * aimX * gyroSensitivity)};
    };
    if (motionSource == 2) {
        GCMotion* sensors = controllerMotion(controller);
        if (!sensors) return {};
        auto rotation = sensors.rotationRate;
        const auto& bias = motionCalibration[1];
        rotation.x -= bias[0]; rotation.y -= bias[1]; rotation.z -= bias[2];
        return sample(rotation, sensors.gravity, sensors.userAcceleration,
                      UIInterfaceOrientationUnknown, switchPro ? -ix : ix);
    }
    const auto state = motion.deviceMotion;
    if (!state) return {};
    auto rotation = state.rotationRate;
    const auto& bias = motionCalibration[0];
    rotation.x -= bias[0]; rotation.y -= bias[1]; rotation.z -= bias[2];
    return sample(rotation, state.gravity, state.userAcceleration,
                  static_cast<UIInterfaceOrientation>(motionOrientation.load()), -ix);
}

static bool sampleFrameInput(GCController* controller, GCExtendedGamepad* pad) {
    static __weak GCController* lastController;
    static bool switchPro;
    if (controller != lastController) {
        lastController = controller;
        switchPro = nintendoController(controller);
        if (controller) NSLog(@"REMASKED_GAMEPAD category=%@ switchPro=%d", controller.productCategory, switchPro);
    }
    auto physical = physicalControllerButtons(pad, switchPro);
    if (launcherConfirmHeldOnStart) {
        launcherConfirmHeldOnStart = pad.buttonA.pressed;
        if (launcherConfirmHeldOnStart)
            physical &= ~Controller::PhysicalBit(switchPro ? Controller::East : Controller::South);
    }
    if (settingsBackHeldOnResume) {
        settingsBackHeldOnResume = pad.buttonB.pressed;
        if (settingsBackHeldOnResume)
            physical &= ~Controller::PhysicalBit(switchPro ? Controller::South : Controller::East);
    }
    const uint16_t buttons = Controller::Map(physical, controllerAssignments);
    const uint16_t settings = Controller::Bit(RETRO_DEVICE_ID_JOYPAD_SELECT);
    const uint16_t pictograph = Controller::Bit(RETRO_DEVICE_ID_JOYPAD_R3);
    gamepadButtons.store(buttons & ~(settings | pictograph), std::memory_order_relaxed);
    const std::array sticks{pad.leftThumbstick, pad.rightThumbstick};
    for (unsigned i = 0; i < sticks.size(); ++i) {
        const auto [x, y] = Controller::Stick(sticks[i].xAxis.value, -sticks[i].yAxis.value,
                                               i == 0 ? movementDeadZone : cameraDeadZone);
        gamepadAxes[2*i].store(axis(x), std::memory_order_relaxed);
        gamepadAxes[2*i+1].store(axis(y), std::memory_order_relaxed);
    }
    gamepadPictograph.store((buttons & pictograph) != 0, std::memory_order_relaxed);
    const auto sensors = sampleMotion(controller, switchPro);
    for (unsigned i = 0; i < sensors.size(); ++i)
        motionValues[i].store(sensors[i], std::memory_order_relaxed);
    return (buttons & settings) != 0;
}

static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port) return 0;
    if (device == RETRO_DEVICE_JOYPAD) {
        const uint16_t bits = touchButtons | gamepadButtons.load(std::memory_order_relaxed);
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(bits);
        return id < 16 && (bits & (1u << id)) != 0;
    }
    if (device == RETRO_DEVICE_ANALOG && id < 2) {
        const bool right = index == RETRO_DEVICE_INDEX_ANALOG_RIGHT;
        const int16_t touched = right ? (id ? lookY : lookX) : (id ? moveY : moveX);
        return touched ? touched : gamepadAxes[(right ? 2 : 0) + id].load(std::memory_order_relaxed);
    }
    if (device == RETRO_DEVICE_POINTER && !index && airPlayDual && bottomTouchPressed) {
        if (id == RETRO_DEVICE_ID_POINTER_PRESSED || id == RETRO_DEVICE_ID_POINTER_COUNT) return 1;
        if (id == RETRO_DEVICE_ID_POINTER_X) return bottomTouchX;
        if (id == RETRO_DEVICE_ID_POINTER_Y) return bottomTouchY;
    }
    return 0;
}

static void coreLog(retro_log_level level, const char* format, ...) {
    if (level < RETRO_LOG_WARN) return;
    char line[1536];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    NSLog(@"Azahar: %s", line);
}
static bool environment(unsigned command, void* data) {
    switch (command) {
    case RemaskedGetSettings: {
        auto& settings = *static_cast<RemaskedGameSettings*>(data);
        settings = gameSettings;
        settings.singleScreen = !airPlayDual;
        return true;
    }
    case RemaskedGetPictograph: {
        const bool held = gamepadPictograph.load(std::memory_order_relaxed);
        const bool touch = touchPictograph.exchange(false);
        *static_cast<bool*>(data) = touch || (held && !pictographStickHeld);
        pictographStickHeld = held;
        return true;
    }
    case RemaskedGetPicture:
        *static_cast<RemaskedPictureSettings*>(data) = pictureSettings;
        return true;
    case RemaskedGetTouchHud:
        *static_cast<MM3D::TouchHud::State*>(data) = landscapeArtwork && !airPlayScreen
            ? touchHud : MM3D::TouchHud::State{};
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = dataDirectory.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto& request = *static_cast<retro_variable*>(data);
        const auto it = options.find(request.key);
        request.value = it == options.end() ? nullptr : it->second.c_str();
        return it != options.end();
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = std::exchange(optionsChanged, false); return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *static_cast<bool*>(data) = true; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *static_cast<unsigned*>(data) = 2; return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
        const auto* definitions = static_cast<const retro_core_options_v2*>(data)->definitions;
        if (definitions) for (auto entry = definitions; entry->key; ++entry) {
            if (entry->default_value) options.try_emplace(entry->key, entry->default_value);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = coreLog; return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_VULKAN; return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        hardware = *static_cast<retro_hw_render_callback*>(data);
        return hardware.context_type == RETRO_HW_CONTEXT_VULKAN;
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
        negotiation = *static_cast<retro_hw_render_context_negotiation_interface_vulkan*>(data); return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        *static_cast<const retro_hw_render_interface_vulkan**>(data) = &video.interface;
        return video.ready();
    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE: {
        auto& sensor = *static_cast<retro_sensor_interface*>(data);
        sensor.set_sensor_state = [](unsigned port, retro_sensor_action, unsigned) {
            return port == 0;
        };
        sensor.get_sensor_input = [](unsigned port, unsigned id) -> float {
            return port || id >= motionValues.size() ? 0 : motionValues[id].load(std::memory_order_relaxed);
        };
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        coreMessage = static_cast<retro_message*>(data)->msg; return true;
    default: return false;
    }
}

static size_t audio(const int16_t* samples, size_t count) {
    if (!audioPlayer || !audioFormat || !count) return count;
    if (count > 8192) return count;
    auto buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:audioFormat frameCapacity:AVAudioFrameCount(count)];
    buffer.frameLength = AVAudioFrameCount(count);
    auto channels = buffer.floatChannelData;
    for (size_t i = 0; i < count; ++i) {
        channels[0][i] = samples[2*i] / 32768.f;
        channels[1][i] = samples[2*i+1] / 32768.f;
    }
    [audioPlayer scheduleBuffer:buffer completionHandler:nil];
    return count;
}

static void setRetroTouchButton(unsigned button, bool pressed) {
    const auto bit = uint16_t(1u << button);
    if (pressed) touchButtons.fetch_or(bit);
    else touchButtons.fetch_and(uint16_t(~bit));
}

void ReMaskedIOSTouchButton(int button, bool pressed) {
    if (pressed && touchVibration) {
        UIImpactFeedbackGenerator* feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        [feedback impactOccurred];
    }
    if (button == ReMaskedTouchPictograph) {
        if (pressed) touchPictograph = true;
        return;
    }
    constexpr int mapping[] = {
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
        -1, -1, RETRO_DEVICE_ID_JOYPAD_START, -1, -1,
        RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT
    };
    if (button == 4 || button == 7) {
        if (pressed && touchSettings) dispatch_async(dispatch_get_main_queue(), touchSettings);
        return;
    }
    if (button >= 0 && button < int(std::size(mapping)) && mapping[button] >= 0)
        setRetroTouchButton(unsigned(mapping[button]), pressed);
}

void ReMaskedIOSTouchAxis(int axis, int16_t value) {
    switch (axis) {
    case 0: moveX = value; break;
    case 1: moveY = value; break;
    case 2: lookX = value; break;
    case 3: lookY = value; break;
    case 4: setRetroTouchButton(RETRO_DEVICE_ID_JOYPAD_L, value > 0); break;
    case 5: setRetroTouchButton(RETRO_DEVICE_ID_JOYPAD_R, value > 0); break;
    }
}

void ReMaskedAzaharReleaseTouch() {
    touchButtons = 0;
    moveX = 0; moveY = 0; lookX = 0; lookY = 0;
    touchPictograph = false;
}

CGRect ReMaskedAzaharTouchCanvas(CGRect safe) {
    if (airPlayScreen) return safe;
    const CGRect controls = CGRectIntersection(safe, touchPicture);
    return CGRectIsEmpty(controls) ? RetroVideo::fit(safe, 400, 240) : controls;
}

CGRect ReMaskedAzaharBottomPicture() { return bottomPicture; }

@interface ReMaskedBottomTouchView : UIView
@property(nonatomic,weak) UITouch* activeTouch;
- (void)cancelTouch;
@end

@implementation ReMaskedBottomTouchView
- (void)updateTouch:(UITouch*)touch {
    if (!airPlayDual || CGRectIsEmpty(self.bounds)) return;
    const auto point = [touch locationInView:self];
    const CGFloat x = std::clamp(point.x / CGRectGetWidth(self.bounds), .5 / 320, 319.5 / 320);
    const CGFloat y = std::clamp(point.y / CGRectGetHeight(self.bounds), .5 / 240, 239.5 / 240);
    // Libretro's pointer is normalized over the full 400×480 framebuffer.
    bottomTouchX = axis(.8 * (2*x - 1));
    bottomTouchY = axis(y);
    bottomTouchPressed = true;
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (self.activeTouch) return;
    self.activeTouch = touches.anyObject;
    [self updateTouch:self.activeTouch];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (self.activeTouch && [touches containsObject:self.activeTouch]) [self updateTouch:self.activeTouch];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (self.activeTouch && [touches containsObject:self.activeTouch]) [self cancelTouch];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}
- (void)cancelTouch {
    self.activeTouch = nil;
    bottomTouchPressed = false;
}
@end

void ReMaskedAzaharPublishTouchHud(const CGRect* controls) {
    touchHud = {};
    if (!controls || CGRectIsEmpty(touchPicture)) return;
    touchHud.enabled = true;
    for (unsigned i = 0; i < touchHud.anchors.size(); ++i) {
        const CGRect control = controls[i];
        if (CGRectIsEmpty(control)) continue;
        touchHud.anchors[i] = MM3D::TouchHud::Project(
            (CGRectGetMidX(control) - CGRectGetMinX(touchPicture)) / touchPicture.size.width,
            (CGRectGetMidY(control) - CGRectGetMinY(touchPicture)) / touchPicture.size.height,
            std::min(control.size.width, control.size.height) / touchPicture.size.width,
            touchPicture.size.width, touchPicture.size.height, 0, 0,
            touchPicture.size.width, touchPicture.size.height, false);
        auto& anchor = touchHud.anchors[i];
        const float expand = video.topPictureAspect() / RemaskedNativeAspect;
        anchor.x = 200.f + (anchor.x - 200.f) * expand;
    }
}

@interface ReMaskedPrototype : UIViewController <MTKViewDelegate, UIDocumentPickerDelegate> {
    std::unique_ptr<Menu::Model> _launcherModel;
    double _launcherInputTime;
}
@property(nonatomic,strong) NSURL* root;
@property(nonatomic,strong) MTKView* screen;
@property(nonatomic,strong) UIView* airPlayBackdrop;
@property(nonatomic,strong) ReMaskedBottomTouchView* bottomTouchView;
@property(nonatomic,strong) UIView* launcher;
@property(nonatomic,strong) UIImageView* launcherMasks;
@property(nonatomic,strong) UILabel* launcherTitle;
@property(nonatomic,strong) UIStackView* launcherButtons;
@property(nonatomic,strong) CADisplayLink* launcherInputLink;
@property(nonatomic) BOOL settingsButtonHeld;
@property(nonatomic,strong) UILabel* status;
@property(nonatomic,strong) UIButton* start;
@property(nonatomic,copy) NSString* pickerPurpose;
@property(nonatomic) BOOL live;
@property(nonatomic) BOOL syncingDrive;
@property(nonatomic) BOOL foregroundSaveCheckPending;
@property(nonatomic) BOOL cloudSaveReloadAvailable;
@property(nonatomic) double last;
@property(nonatomic) NSUInteger frames;
@property(nonatomic) NSUInteger hitches;
@property(nonatomic) double worst;
- (void)startGame;
- (void)openSettings;
- (void)importSaves;
- (void)exportSaves;
- (void)prepareDriveSaves;
- (void)refreshSavesAfterForeground;
- (void)offerCloudSaveReload;
- (void)backUpDriveSaves;
- (void)updateAirPlay;
- (void)updateAirPlayIdleTimer;
- (void)syncDrive:(ReMaskedDrive::Content)content
       resolution:(ReMaskedDrive::Resolution)resolution showResult:(BOOL)showResult;
- (Menu::Document)settingsDocument;
@end

static UIColor* LauncherSelectionColor() {
    return [UIColor colorWithRed:185/255.0 green:125/255.0 blue:242/255.0 alpha:1];
}

@interface ReMaskedLauncherButton : UIButton
@property(nonatomic,strong) CAGradientLayer* selectionBanner;
@end

@implementation ReMaskedLauncherButton
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.selectionBanner = [CAGradientLayer layer];
    self.selectionBanner.colors = @[(id)UIColor.clearColor.CGColor,
        (id)[LauncherSelectionColor() colorWithAlphaComponent:.20].CGColor];
    self.selectionBanner.startPoint = CGPointMake(0,.5);
    self.selectionBanner.endPoint = CGPointMake(1,.5);
    self.selectionBanner.cornerRadius = 4;
    self.selectionBanner.hidden = YES;
    [self.layer insertSublayer:self.selectionBanner atIndex:0];
    [self setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [self setTitleColor:LauncherSelectionColor() forState:UIControlStateSelected];
    [self setTitleColor:LauncherSelectionColor() forState:UIControlStateHighlighted];
    [self setTitleColor:LauncherSelectionColor() forState:UIControlStateSelected | UIControlStateHighlighted];
    [self setTitleColor:UIColor.lightGrayColor forState:UIControlStateDisabled];
    return self;
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.selectionBanner.hidden = !(self.selected || self.highlighted);
}
- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    self.selectionBanner.hidden = !(self.selected || self.highlighted);
}
- (void)layoutSubviews {
    [super layoutSubviews];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.selectionBanner.frame = self.bounds;
    [CATransaction commit];
}
@end

static __weak ReMaskedPrototype* airPlayHost;

@implementation ReMaskedPrototype
- (UIButton*)button:(NSString*)label action:(SEL)action {
    UIButton* button = [[ReMaskedLauncherButton alloc] initWithFrame:CGRectZero];
    button.titleLabel.font = [UIFont fontWithName:@"FiraSans-Bold" size:17] ?: [UIFont boldSystemFontOfSize:17];
    button.contentEdgeInsets = UIEdgeInsetsMake(0,12,0,12);
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentRight;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [button setTitle:label forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.root = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask] firstObject];
    LoadGamePreferences();
    dataDirectory = self.root.fileSystemRepresentation;
    self.screen = [[MTKView alloc] initWithFrame:self.view.bounds device:MTLCreateSystemDefaultDevice()];
    self.screen.framebufferOnly = YES;
    self.screen.clearColor = MTLClearColorMake(0,0,0,1);
    self.screen.preferredFramesPerSecond = 60;
    self.screen.delegate = self;
    self.screen.paused = YES;
    [self.view addSubview:self.screen];
    self.airPlayBackdrop = [UIView new];
    self.airPlayBackdrop.backgroundColor = UIColor.blackColor;
    self.airPlayBackdrop.userInteractionEnabled = NO;
    self.airPlayBackdrop.hidden = YES;
    [self.view addSubview:self.airPlayBackdrop];
    self.bottomTouchView = [ReMaskedBottomTouchView new];
    self.bottomTouchView.hidden = YES;
    [self.view addSubview:self.bottomTouchView];
    __weak ReMaskedPrototype* host = self;
    touchSettings = ^{ [host openSettings]; };
    self.launcher = [UIView new];
    self.launcher.backgroundColor = [UIColor colorWithRed:8/255.0 green:7/255.0 blue:13/255.0 alpha:1];
    [self.view addSubview:self.launcher];

    self.launcherMasks = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"ReMaskedMasks.png"]];
    self.launcherMasks.contentMode = UIViewContentModeScaleAspectFit;
    [self.launcher addSubview:self.launcherMasks];

    self.launcherTitle = [UILabel new];
    self.launcherTitle.text = @"Majora ReMasked";
    self.launcherTitle.font = [UIFont fontWithName:@"FiraSans-Bold" size:34] ?: [UIFont boldSystemFontOfSize:34];
    self.launcherTitle.textColor = UIColor.whiteColor;
    self.launcherTitle.adjustsFontSizeToFitWidth = YES;
    self.launcherTitle.minimumScaleFactor = .65;
    [self.launcher addSubview:self.launcherTitle];

    self.status = [UILabel new];
    self.status.font = [UIFont systemFontOfSize:13];
    self.status.textColor = UIColor.lightGrayColor;
    self.status.numberOfLines = 2;
    [self.launcher addSubview:self.status];
    self.start = [self button:@"Start game" action:@selector(startGame)];
    UIButton* settings = [self button:@"Settings" action:@selector(openSettings)];
    self.launcherButtons = [[UIStackView alloc] initWithArrangedSubviews:@[self.start,settings]];
    self.launcherButtons.axis = UILayoutConstraintAxisVertical;
    self.launcherButtons.distribution = UIStackViewDistributionFillEqually;
    self.launcherButtons.spacing = 6;
    [self.launcher addSubview:self.launcherButtons];

    Menu::Tab actions;
    for (UIButton* button in self.launcherButtons.arrangedSubviews) {
        __weak UIButton* action = button;
        Menu::Row row;
        row.Kind = Menu::RowKind::Action;
        row.Activate = [action] {
            if (action.enabled) [action sendActionsForControlEvents:UIControlEventTouchUpInside];
            return Menu::WriteResult{};
        };
        actions.Rows.push_back(std::move(row));
    }
    Menu::Document launcherMenu;
    launcherMenu.Tabs.push_back(std::move(actions));
    _launcherModel = std::make_unique<Menu::Model>(std::move(launcherMenu));
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!self.launcher.hidden) [self startLauncherInput];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [self stopLauncherInput];
}

- (void)startLauncherInput {
    if (self.launcherInputLink) return;
    _launcherModel->Open();
    _launcherInputTime = 0;
    self.launcherInputLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(updateLauncherInput:)];
    [self.launcherInputLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)stopLauncherInput {
    [self.launcherInputLink invalidate];
    self.launcherInputLink = nil;
}

- (void)updateLauncherInput:(CADisplayLink*)sender {
    const double elapsed = _launcherInputTime ? std::min(sender.timestamp - _launcherInputTime, .1) : 0;
    _launcherInputTime = sender.timestamp;
    if (self.launcher.hidden || UIApplication.sharedApplication.applicationState != UIApplicationStateActive)
        return;
    const auto connected = menuControllerInput();
    const auto held = connected.value_or(Menu::Input{});
    if (self.presentedViewController) return;
    _launcherModel->Update({.Up = held.Up, .Down = held.Down, .Confirm = held.Confirm}, elapsed);
    const auto focused = _launcherModel->RowIndex();
    NSUInteger index = 0;
    for (UIButton* button in self.launcherButtons.arrangedSubviews) {
        const bool selected = connected && index == focused;
        ++index;
        if (selected != button.selected) button.selected = selected;
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    motionOrientation.store(self.view.window.windowScene.interfaceOrientation);
    self.launcher.frame = self.view.bounds;
    CGRect safe = UIEdgeInsetsInsetRect(self.view.bounds,self.view.safeAreaInsets);
    const CGFloat x = CGRectGetMinX(safe), y = CGRectGetMinY(safe);
    const CGFloat w = CGRectGetWidth(safe), h = CGRectGetHeight(safe);
    self.airPlayBackdrop.frame = self.view.bounds;
    const bool upright = h > w;
    self.launcherTitle.textAlignment = NSTextAlignmentRight;
    self.status.textAlignment = upright ? NSTextAlignmentLeft : NSTextAlignmentRight;
    for (UIButton* button in self.launcherButtons.arrangedSubviews)
        button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentRight;

    if (upright) {
        self.launcherTitle.font = [UIFont boldSystemFontOfSize:24];
        self.launcherTitle.frame = CGRectMake(x+w*.31,y+12,w*.64,50);
        self.launcherMasks.frame = CGRectMake(x+w*.02,y+h*.16,w*.56,h*.78);
        self.launcherButtons.spacing = 18;
        self.launcherButtons.frame = CGRectMake(x+w*.36,y+std::max(110.0,h-144),w*.59,114);
        self.status.textColor = self.launcherTitle.textColor;
        self.status.frame = CGRectMake(x+16,y+h-42,w-32,40);
    } else {
        self.launcherTitle.font = [UIFont fontWithName:@"FiraSans-Bold" size:34] ?:
            [UIFont boldSystemFontOfSize:34];
        const CGFloat titleX = x+w*.34, titleWidth = w*.61;
        const CGFloat buttonHeight = 94, buttonY = y+h-54-buttonHeight;
        self.launcherTitle.frame = CGRectMake(titleX,y+10,titleWidth,48);
        self.launcherMasks.frame = CGRectMake(x+w*.02,y+h*.08,w*.53,h*.84);
        self.launcherButtons.spacing = 6;
        self.launcherButtons.frame = CGRectMake(titleX,buttonY,titleWidth,buttonHeight);
        self.status.textColor = UIColor.lightGrayColor;
        self.status.frame = CGRectMake(titleX,buttonY+buttonHeight+5,titleWidth,42);
    }
    auto rect = [](CGRect r) { return ReMaskedDuo::Rect{float(r.origin.x),float(r.origin.y),
        float(r.size.width),float(r.size.height)}; };
    ReMaskedIOS::PhonePortrait.Publish(rect(self.view.bounds),rect(safe),
        (!airPlayScreen || airPlayDual) && UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone,
        airPlayDual ? 4.f / 3.f : 16.f / 9.f);
    auto portrait = ReMaskedIOS::PhonePortrait.Pixels(self.view.bounds.size.width,self.view.bounds.size.height);
    const bool fullLandscape = self.view.bounds.size.width > self.view.bounds.size.height &&
        Preference("PictureAspect", 0) == 3 && !Preference("SoftwareRenderer", 0);
    const CGFloat rail = airPlayDual ? std::min<CGFloat>(132, w*.24) : 0;
    self.screen.frame = portrait.Active() ? CGRectMake(portrait.Game.X,portrait.Game.Y,
        portrait.Game.Width,portrait.Game.Height) : airPlayScreen ? CGRectInset(safe,rail,0)
        : fullLandscape ? self.view.bounds : safe;
    self.bottomTouchView.frame = RetroVideo::fit(self.screen.frame, 320, 240);
    [self updateTouchPicture];
}

- (void)updateTouchPicture {
    const int aspectMode = std::clamp(Preference("PictureAspect", 0), 0, 3);
    const CGSize size = (airPlayScreen ?: self.screen).bounds.size;
    const float freeAspect = size.height > 0 ? size.width / size.height : RemaskedNativeAspect;
    const float wanted = aspectMode < int(PictureAspects.size()) ? PictureAspects[aspectMode] : freeAspect;
    video.setPictureAspect(Preference("SoftwareRenderer", 0) ? RemaskedNativeAspect
                           : std::clamp(wanted, RemaskedNativeAspect, 3.f));
    const CGRect frame = [self.screen convertRect:self.screen.bounds toView:self.view.window];
    const CGRect next = video.picture(frame);
    const CGRect safe = [self.view convertRect:self.view.safeAreaLayoutGuide.layoutFrame toView:self.view.window];
    pictureSettings = {video.topPictureAspect()};
    if (!airPlayScreen && next.size.height > 0) {
        const CGFloat units = 240. / next.size.height;
        pictureSettings.leftInset = std::max(0., CGRectGetMinX(safe) - CGRectGetMinX(next)) * units;
        pictureSettings.rightInset = std::max(0., CGRectGetMaxX(next) - CGRectGetMaxX(safe)) * units;
    }
    const CGRect bottom = airPlayDual ? [self.view convertRect:self.bottomTouchView.frame
                                                         toView:self.view.window] : CGRectNull;
    const bool sameBottom = (CGRectIsNull(bottom) && CGRectIsNull(bottomPicture)) ||
                            CGRectEqualToRect(bottom, bottomPicture);
    if (CGRectEqualToRect(touchPicture, next) && sameBottom) return;
    touchPicture = next;
    bottomPicture = bottom;
    // The overlay is attached to the UIWindow rather than the game view.
    ReMaskedIOS_RefreshTouchLayout();
}

static constexpr std::array<const char*, 13> TouchControlNames{
    "Action", "Sword", "Item X", "Item Y", "Item I", "Item II", "Target", "Shield",
    "Start / Items menu", "Select / App settings", "D-pad", "Movement stick", "Camera stick"
};

- (void)showTouch {
    ReMaskedIOS_SetTouchControlsMenuVisible(0);
    ReMaskedIOS_SetTouchControlsEnabled(touchControlsEnabled);
    ReMaskedIOS_SetFaceButtonLayout(std::clamp(Preference("FaceLayout", 0), 0, 2));
    ReMaskedIOS_SetTouchLeftStickFloating(!Preference("FixedStick", 0));
    ReMaskedIOS_SetTouchOpacity(Preference("TouchOpacity", 28) / 100.f);
    ReMaskedIOS_SetTouchScale(Preference("TouchScale", 100) / 100.f);
    ReMaskedIOS_SetTouchOptions(Preference("ShoulderLabels", 0), Preference("DragLook", 0));
    ReMaskedIOS_SetExtraShoulders(Preference("ExtraShoulders", 1));
    unsigned hidden = 0;
    for (unsigned i = 0; i < TouchControlNames.size(); ++i) {
        const auto key = std::string("Touch.") + std::to_string(i);
        if (!Preference((key + ".Visible").c_str(), 1)) hidden |= 1u << i;
        ReMaskedIOS_SetControlSize(i, Preference((key + ".Size").c_str(), 100) / 100.f);
    }
    ReMaskedAzaharSetTouchVisibility(hidden, Preference("HideWithController", 0));
}
- (void)updateAirPlay {
    airPlayDual = airPlayScreen && Preference("AirPlayMode", 0) == int(AirPlayMode::TopScreen);
    airPlayScreen.delegate = self;
    const char* layout = airPlayDual ? "default" : "single_screen";
    if (self.live && options["citra_layout_option"] != layout) {
        options["citra_layout_option"] = layout;
        options["citra_enable_touch_touchscreen"] = airPlayDual ? "enabled" : "disabled";
        optionsChanged = true;
    }
    ReMaskedAzaharReleaseTouch();
    ReMaskedAzaharPublishTouchHud(nullptr);
    self.airPlayBackdrop.hidden = !airPlayScreen || airPlayDual;
    [self.bottomTouchView cancelTouch];
    self.bottomTouchView.hidden = !airPlayDual || self.screen.paused;
    [self.view setNeedsLayout];
    if (self.live && !self.screen.paused) [self showTouch];
    ReMaskedIOS_RefreshTouchLayout();
    [self updateAirPlayIdleTimer];
    NSLog(@"REMASKED_AIRPLAY mode=%@",
          !airPlayScreen ? @"local" : airPlayDual ? @"top-screen" : @"single-screen");
}
- (void)updateAirPlayIdleTimer {
    UIApplication.sharedApplication.idleTimerDisabled = airPlayScreen && self.live && !self.screen.paused
        && self.view.window.windowScene.activationState == UISceneActivationStateForegroundActive;
}
- (void)editTouchLayout {
    if (!self.live || airPlayDual) return;
    [self showTouch];
    ReMaskedIOS_SetTouchControlsMenuVisible(1);
    ReMaskedIOS_EditTouchLayout();
}
- (NSURL*)rom { return [self.root URLByAppendingPathComponent:@"Majora.cci"]; }
- (void)chooseROM {
    self.pickerPurpose = @"rom";
    auto picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.delegate = self;
    [(self.presentedViewController ?: self) presentViewController:picker animated:YES completion:nil];
}
- (NSURL*)modelActor {
    return [self.root URLByAppendingPathComponent:
        @"Azahar/load/mods/0004000000125500/romfs/actors/zelda2_link_child_new.gar.lzs"];
}

- (NSURL*)importedActor {
    return [[self.root URLByAppendingPathComponent:@"Models" isDirectory:YES]
        URLByAppendingPathComponent:@"zelda2_link_child_new.gar.lzs"];
}

- (void)choosePreparedModel {
    if (self.live) return;
    self.pickerPurpose = @"model";
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder, UTTypeData] asCopy:NO];
    picker.delegate = self;
    [(self.presentedViewController ?: self) presentViewController:picker animated:YES completion:nil];
}

- (void)installPreparedModel:(NSURL*)selection {
    if (self.live) return;
    const BOOL access = [selection startAccessingSecurityScopedResource];
    __block NSData* actor;
    NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:selection options:0 error:nil byAccessor:^(NSURL* url) {
        NSNumber* directory;
        [url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil];
        if (directory.boolValue)
            url = [url URLByAppendingPathComponent:@"romfs/actors/zelda2_link_child_new.gar.lzs"];
        if (![url.lastPathComponent isEqualToString:@"zelda2_link_child_new.gar.lzs"]) return;
        NSNumber* size;
        if ([url getResourceValue:&size forKey:NSURLFileSizeKey error:nil] &&
            size.unsignedLongLongValue >= 16 && size.unsignedLongLongValue <= 32 * 1024 * 1024)
            actor = [NSData dataWithContentsOfURL:url];
    }];
    if (access) [selection stopAccessingSecurityScopedResource];
    if (actor.length < 16 || std::memcmp(actor.bytes, "LzS", 3)) {
        [self settingsNotice:@"Choose a prepared MM3D model folder with its compressed Link actor, or the prepared actor file."];
        return;
    }
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* target = self.modelActor;
    NSData* existing = [NSData dataWithContentsOfURL:target];
    NSData* recorded = [NSData dataWithContentsOfURL:self.importedActor];
    if ([files fileExistsAtPath:target.path] && (!recorded || ![existing isEqualToData:recorded])) {
        [self settingsNotice:@"The game already has a model override installed outside this menu."];
        return;
    }
    NSError* error;
    if (![files createDirectoryAtURL:target.URLByDeletingLastPathComponent
          withIntermediateDirectories:YES attributes:nil error:&error] ||
        ![files createDirectoryAtURL:self.importedActor.URLByDeletingLastPathComponent
          withIntermediateDirectories:YES attributes:nil error:&error] ||
        ![actor writeToURL:target options:NSDataWritingAtomic error:&error]) {
        [self settingsNotice:error.localizedDescription];
        return;
    }
    if (![actor writeToURL:self.importedActor options:NSDataWritingAtomic error:&error]) {
        if (existing) [existing writeToURL:target options:NSDataWritingAtomic error:nil];
        else [files removeItemAtURL:target error:nil];
        [self settingsNotice:error.localizedDescription];
        return;
    }
    [self settingsNotice:@"The prepared actor will load the next time you start the game."];
}

- (void)restoreOriginalModel {
    if (self.live) return;
    NSURL* target = self.modelActor;
    NSFileManager* files = NSFileManager.defaultManager;
    if (![files fileExistsAtPath:target.path]) {
        [self settingsNotice:@"The original model is already selected."];
        return;
    }
    NSData* imported = [NSData dataWithContentsOfURL:self.importedActor];
    if (!imported || ![imported isEqualToData:[NSData dataWithContentsOfURL:target]]) {
        [self settingsNotice:@"This override was installed outside the ReMasked settings menu."];
        return;
    }
    NSError* error;
    [self settingsNotice:[files removeItemAtURL:target error:&error] ?
        @"Original Link will load the next time you start the game." : error.localizedDescription];
}

- (NSURL*)saveDirectory {
    return [self.root URLByAppendingPathComponent:ReMaskedDrive::AzaharSavesPath() isDirectory:YES];
}

- (void)syncDrive:(ReMaskedDrive::Content)content
       resolution:(ReMaskedDrive::Resolution)resolution showResult:(BOOL)showResult {
    using namespace ReMaskedDrive;
    if (self.syncingDrive || (self.live && content != Content::AzaharBackup)) return;
    if (!Preference("ICloudDriveEnabled", 1)) return;
    self.syncingDrive = YES;
    if (!self.live) {
        self.start.enabled = NO;
        self.status.text = @"Syncing iCloud Drive…";
    }
    auto panel = self.presentedViewController;
    if ([panel isKindOfClass:ReMaskedSettingsPanel.class])
        [(ReMaskedSettingsPanel*)panel refreshSettings];
    ReMaskedDrive::Sync(self.root, nil, resolution, ^(Result result) {
        self.syncingDrive = NO;
        self.start.enabled = YES;
        if (result.ok && content == Content::AzaharData) SavePreference("ICloudDriveInitialized", 1);
        if (!self.live) self.status.text = result.message;
        UIViewController* settings = self.presentedViewController;
        if ([settings isKindOfClass:ReMaskedSettingsPanel.class])
            [(ReMaskedSettingsPanel*)settings refreshSettings];
        if (!result.conflict) {
            if (showResult && UIApplication.sharedApplication.applicationState == UIApplicationStateActive)
                [self settingsNotice:result.message];
            if (self.foregroundSaveCheckPending)
                [self refreshSavesAfterForeground];
            return;
        }
        self.foregroundSaveCheckPending = NO;
        if (self.view.window.windowScene.activationState != UISceneActivationStateForegroundActive) {
            self.foregroundSaveCheckPending = YES;
            return;
        }
        UIAlertController* conflict = [UIAlertController alertControllerWithTitle:@"Drive sync conflict"
            message:result.message preferredStyle:UIAlertControllerStyleAlert];
        [conflict addAction:[UIAlertAction actionWithTitle:@"Keep local copies" style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*) { [self syncDrive:content resolution:Resolution::Local showResult:YES]; }]];
        [conflict addAction:[UIAlertAction actionWithTitle:@"Keep Drive copies" style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*) { [self syncDrive:content resolution:Resolution::Drive showResult:YES]; }]];
        [conflict addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [(self.presentedViewController ?: self) presentViewController:conflict animated:YES completion:nil];
    }, content);
}

- (void)prepareDriveSaves {
    const auto content = Preference("ICloudDriveInitialized", 0)
        ? ReMaskedDrive::Content::AzaharSaves : ReMaskedDrive::Content::AzaharData;
    [self syncDrive:content resolution:ReMaskedDrive::Resolution::Ask showResult:NO];
}

- (void)refreshSavesAfterForeground {
    using namespace ReMaskedDrive;
    if (!Preference("ICloudDriveEnabled", 1)) {
        self.foregroundSaveCheckPending = NO;
        return;
    }
    self.foregroundSaveCheckPending = YES;
    if (self.syncingDrive ||
        self.view.window.windowScene.activationState != UISceneActivationStateForegroundActive)
        return;
    self.foregroundSaveCheckPending = NO;
    if (!self.live) {
        [self syncDrive:Content::AzaharSaves resolution:Resolution::Ask showResult:NO];
        return;
    }
    self.syncingDrive = YES;
    ReMaskedDrive::Sync(self.root, nil, Resolution::Ask, ^(Result result) {
        self.syncingDrive = NO;
        self.cloudSaveReloadAvailable = result.ok && result.pendingImport;
        if (self.foregroundSaveCheckPending) [self refreshSavesAfterForeground];
        else [self offerCloudSaveReload];
    }, Content::AzaharCheck);
}

- (void)offerCloudSaveReload {
    if (!self.cloudSaveReloadAvailable || self.presentedViewController || self.syncingDrive ||
        self.view.window.windowScene.activationState != UISceneActivationStateForegroundActive)
        return;
    self.cloudSaveReloadAvailable = NO;
    const BOOL wasPlaying = !self.screen.paused;
    if (wasPlaying) [self pauseGame];
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"New iCloud save available"
        message:@"Restart the game to use the newer save. Unsaved progress in this session will be lost."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Keep playing" style:UIAlertActionStyleCancel
        handler:^(UIAlertAction*) { if (wasPlaying) [self startGame]; }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Restart and sync" style:UIAlertActionStyleDefault
        handler:^(UIAlertAction*) {
            [audioEngine stop];
            audioPlayer = nil;
            audioEngine = nil;
            retro_unload_game();
            retro_deinit();
            video.stop();
            hardware = {};
            negotiation = {};
            self.live = NO;
            self.frames = 0;
            self.last = 0;
            [self.start setTitle:@"Start game" forState:UIControlStateNormal];
            [self refreshSavesAfterForeground];
        }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)backUpDriveSaves {
    if (!self.live || self.syncingDrive || !Preference("ICloudDriveEnabled", 1)) return;
    using namespace ReMaskedDrive;
    self.syncingDrive = YES;
    UIApplication* app = UIApplication.sharedApplication;
    __block UIBackgroundTaskIdentifier background = UIBackgroundTaskInvalid;
    void (^finish)(void) = ^{
        if (background == UIBackgroundTaskInvalid) return;
        [app endBackgroundTask:background];
        background = UIBackgroundTaskInvalid;
    };
    background = [app beginBackgroundTaskWithName:@"ReMasked save sync" expirationHandler:finish];
    ReMaskedDrive::Sync(self.root, nil, Resolution::Ask, ^(Result) {
        self.syncingDrive = NO;
        finish();
        if (self.foregroundSaveCheckPending) [self refreshSavesAfterForeground];
    }, Content::AzaharBackup);
}

- (void)settingsNotice:(NSString*)message {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Majora ReMasked"
        message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [(self.presentedViewController ?: self) presentViewController:alert animated:YES completion:nil];
}

- (void)importSaves {
    if (self.live) return;
    self.pickerPurpose = @"saves";
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.allowsMultipleSelection = YES;
    picker.delegate = self;
    [(self.presentedViewController ?: self) presentViewController:picker animated:YES completion:nil];
}

- (void)exportSaves {
    if (self.live) return;
    NSMutableArray<NSURL*>* files = [NSMutableArray array];
    for (unsigned i = 0; i < 3; ++i) {
        NSURL* slot = [self.saveDirectory URLByAppendingPathComponent:[NSString stringWithFormat:@"save%u.bin", i]];
        if ([NSFileManager.defaultManager fileExistsAtPath:slot.path]) [files addObject:slot];
    }
    if (!files.count) { [self settingsNotice:@"There are no save slots to export."]; return; }
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForExportingURLs:files asCopy:YES];
    [(self.presentedViewController ?: self) presentViewController:picker animated:YES completion:nil];
}

- (void)stageSaveImport:(NSArray<NSURL*>*)urls {
    if (self.live || !urls.count) return;
    if (![NSFileManager.defaultManager fileExistsAtPath:self.saveDirectory.path]) {
        [self settingsNotice:@"Start the game once to initialize its save storage, then reopen the app and import from startup Settings."];
        return;
    }
    NSMutableDictionary<NSString*, NSData*>* slots = [NSMutableDictionary dictionary];
    for (NSURL* url in urls) {
        NSString* name = url.lastPathComponent;
        if (![@[@"save0.bin", @"save1.bin", @"save2.bin"] containsObject:name] || slots[name]) {
            [self settingsNotice:@"Choose distinct save0.bin, save1.bin or save2.bin files."];
            return;
        }
        const BOOL access = [url startAccessingSecurityScopedResource];
        __block NSData* slot;
        NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [coordinator coordinateReadingItemAtURL:url options:0 error:nil byAccessor:^(NSURL* file) {
            NSNumber* size;
            if ([file getResourceValue:&size forKey:NSURLFileSizeKey error:nil] && size.unsignedIntegerValue == 79384)
                slot = [NSData dataWithContentsOfURL:file];
        }];
        if (access) [url stopAccessingSecurityScopedResource];
        if (slot.length != 79384) {
            [self settingsNotice:@"One of the selected files is unreadable or is an invalid MM3D save slot."];
            return;
        }
        slots[name] = slot;
    }
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Import saves?"
        message:@"The selected slots will replace any saves in the same slots. Existing copies will be kept in Save Backups."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Import" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        NSFileManager* files = NSFileManager.defaultManager;
        NSURL* backup = [[self.root URLByAppendingPathComponent:@"Save Backups" isDirectory:YES]
            URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        NSError* error;
        if (![files createDirectoryAtURL:backup withIntermediateDirectories:YES attributes:nil error:&error]) {
            [self settingsNotice:error.localizedDescription];
            return;
        }
        for (NSString* name in slots) {
            NSURL* target = [self.saveDirectory URLByAppendingPathComponent:name];
            if ([files fileExistsAtPath:target.path] &&
                ![files copyItemAtURL:target toURL:[backup URLByAppendingPathComponent:name] error:&error]) {
                [self settingsNotice:error.localizedDescription];
                return;
            }
        }
        NSMutableArray<NSString*>* written = [NSMutableArray array];
        for (NSString* name in slots) {
            NSURL* target = [self.saveDirectory URLByAppendingPathComponent:name];
            if (![slots[name] writeToURL:target options:NSDataWritingAtomic error:&error]) {
                for (NSString* previous in written) {
                    NSURL* path = [self.saveDirectory URLByAppendingPathComponent:previous];
                    NSData* original = [NSData dataWithContentsOfURL:[backup URLByAppendingPathComponent:previous]];
                    if (original) [original writeToURL:path options:NSDataWritingAtomic error:nil];
                    else [files removeItemAtURL:path error:nil];
                }
                [self settingsNotice:error.localizedDescription];
                return;
            }
            [written addObject:name];
        }
        [self settingsNotice:@"Selected save slots imported."];
    }]];
    [(self.presentedViewController ?: self) presentViewController:alert animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if ([self.pickerPurpose isEqualToString:@"model"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ if (urls.count) [self installPreparedModel:urls.firstObject]; });
        return;
    }
    if ([self.pickerPurpose isEqualToString:@"saves"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ [self stageSaveImport:urls]; });
        return;
    }
    NSURL* inputURL = urls.firstObject;
    if (!inputURL) return;
    if ([NSFileManager.defaultManager fileExistsAtPath:self.rom.path]) {
        self.status.text = @"A local ROM is already imported.";
        return;
    }
    BOOL access = [inputURL startAccessingSecurityScopedResource];
    NSError* error = nil;
    BOOL ok = [NSFileManager.defaultManager copyItemAtURL:inputURL toURL:self.rom error:&error];
    if (access) [inputURL stopAccessingSecurityScopedResource];
    self.status.text = ok ? @"ROM ready" : error.localizedDescription;
}
- (void)startGame {
    if (self.syncingDrive && !self.live) return;
    if (self.live) {
        launcherConfirmHeldOnStart = gameplayController().extendedGamepad.buttonA.pressed;
        [self stopLauncherInput];
        self.bottomTouchView.hidden = !airPlayDual;
        self.launcher.hidden = YES; [self showTouch]; self.screen.paused = NO;
        [self updateAirPlayIdleTimer];
        return;
    }
    if (![NSFileManager.defaultManager fileExistsAtPath:self.rom.path]) { [self chooseROM]; return; }
    coreMessage.clear();
    const bool software = Preference("SoftwareRenderer", 0);
    [self updateTouchPicture];
    options = {
        {"citra_graphics_api",software?"Software":"Vulkan"},
        {"citra_use_fastinterp","enabled"},{"citra_layout_option",airPlayDual ? "default" : "single_screen"},
        {"citra_use_hw_shader","enabled"},{"citra_use_disk_shader_cache","enabled"},
        {"citra_resolution_factor","1"},{"citra_custom_textures","enabled"},
        {"citra_use_libretro_save_path","LibRetro Default"},{"citra_enable_motion","enabled"},
        {"citra_enable_touch_touchscreen",airPlayDual ? "enabled" : "disabled"},{"citra_enable_mouse_touchscreen","disabled"},
        {"citra_analog_function","c_stick"}
    };
    UpdateGraphicsOptions();
    optionsChanged = false;
    retro_set_environment(environment);
    retro_set_input_poll([]{});
    retro_set_input_state(input);
    retro_set_video_refresh([](const void* data, unsigned w, unsigned h, size_t pitch) {
        if (data != RETRO_HW_FRAME_BUFFER_VALID) video.softwareFrame(data,w,h,pitch);
    });
    retro_set_audio_sample([](int16_t left,int16_t right) { int16_t in[]={left,right}; audio(in,1); });
    retro_set_audio_sample_batch(audio);
    retro_init();
    const retro_game_info game{ self.rom.fileSystemRepresentation,nullptr,0,nullptr };
    const bool loaded = retro_load_game(&game);
    if (!loaded || (!software && (!hardware.context_reset || !video.start(negotiation)))) {
        if (loaded) retro_unload_game();
        retro_deinit();
        video.stop();
        self.status.text = coreMessage.empty() ? @"Azahar could not start the renderer" : @(coreMessage.c_str());
        return;
    }
    if (!software) hardware.context_reset();
    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    const double rate = info.timing.sample_rate > 0 ? info.timing.sample_rate : 32768;
    audioFormat = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate channels:2];
    audioEngine = [AVAudioEngine new];
    audioPlayer = [AVAudioPlayerNode new];
    [audioEngine attachNode:audioPlayer];
    [audioEngine connect:audioPlayer to:audioEngine.mainMixerNode format:audioFormat];
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryAmbient error:nil];
    [AVAudioSession.sharedInstance setActive:YES error:nil];
    audioPlayer.volume = Preference("Muted", 0) ? 0.f
        : std::clamp(Preference("Volume", 100), 0, 100) / 100.f;
    if ([audioEngine startAndReturnError:nil]) [audioPlayer play];
    else audioPlayer = nil;
    startDeviceMotion();
    loadMotionCalibration();
    self.live = YES;
    launcherConfirmHeldOnStart = gameplayController().extendedGamepad.buttonA.pressed;
    [self stopLauncherInput];
    self.start.enabled = YES;
    [self.start setTitle:@"Resume game" forState:UIControlStateNormal];
    self.launcher.hidden = YES;
    self.bottomTouchView.hidden = !airPlayDual;
    [self showTouch];
    self.screen.preferredFramesPerSecond = info.timing.fps > 0 ? std::clamp((int)lround(info.timing.fps),30,60) : 60;
    self.screen.paused = NO;
    [self updateAirPlayIdleTimer];
    NSLog(@"REMASKED_AZAHAR_START Vulkan=%d coreHz=%.2f", !software,info.timing.fps);
}
- (void)pauseGame {
    self.screen.paused = YES;
    [self updateAirPlayIdleTimer];
    self.bottomTouchView.hidden = YES;
    [self.bottomTouchView cancelTouch];
    ReMaskedIOS_SetTouchControlsMenuVisible(1);
    ReMaskedAzaharPublishTouchHud(nullptr);
    ReMaskedAzaharReleaseTouch();
    self.status.text = @"Game paused";
    self.launcher.hidden = NO;
    [self startLauncherInput];
    self.last = 0;
}
- (void)openSettings {
    if (self.presentedViewController) return;
    if (self.live) [self pauseGame];
    __weak ReMaskedPrototype* host = self;
    ReMaskedSettingsPanel* panel =
        [[ReMaskedSettingsPanel alloc] initWithDocument:[self settingsDocument]
            input:menuControllerInput inGame:self.live close:^{
            settingsBackHeldOnResume = gameplayController().extendedGamepad.buttonB.pressed;
            if (host.live) {
                [host startGame];
                [host offerCloudSaveReload];
            }
        }];
    panel.modalPresentationStyle = UIModalPresentationFullScreen;
    [self presentViewController:panel animated:NO completion:nil];
    NSLog(@"REMASKED_AZAHAR_SETTINGS running=%d",self.live);
}

- (void)calibrateMotion {
    const bool external = (motionSource ? motionSource : Preference("MotionDevice", 1)) == 2;
    GCController* controller = external ? gameplayController() : nil;
    GCMotion* sensors = controllerMotion(controller);
    if (!external) startDeviceMotion();

    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Calibrate motion"
        message:@"Keep the device or controller still." preferredStyle:UIAlertControllerStyleAlert];
    [self.presentedViewController presentViewController:alert animated:YES completion:nil];
    if ((external && !sensors) || (!external && !motion.deviceMotionAvailable)) {
        alert.message = external ? @"Connect a controller with motion sensors." : @"Device motion is unavailable.";
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        return;
    }

    __block NSTimer* timer;
    __block unsigned samples = 0;
    __block double stamp = 0;
    auto sum = std::make_shared<std::array<double, 3>>();
    const auto deadline = CACurrentMediaTime() + 3;
    __weak UIAlertController* weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction*) {
        [timer invalidate];
        timer = nil;
    }]];
    timer = [NSTimer scheduledTimerWithTimeInterval:.025 repeats:YES block:^(NSTimer* tick) {
        auto dialog = weakAlert;
        CMDeviceMotion* state = external ? nil : motion.deviceMotion;
        const auto rotation = external ? CMRotationRate{sensors.rotationRate.x, sensors.rotationRate.y,
            sensors.rotationRate.z} : state.rotationRate;
        const bool present = external ? [GCController.controllers containsObject:controller] : state != nil;
        if (present && (external || stamp != state.timestamp)) {
            stamp = state.timestamp;
            if (std::hypot(rotation.x, rotation.y, rotation.z) < .09) {
                ++samples;
                (*sum)[0] += rotation.x;
                (*sum)[1] += rotation.y;
                (*sum)[2] += rotation.z;
            } else {
                samples = 0;
                sum->fill(0);
            }
        }
        if (dialog && samples < 40 && CACurrentMediaTime() < deadline) return;
        [tick invalidate];
        timer = nil;
        if (!dialog) return;
        if (samples >= 40) {
            auto& bias = motionCalibration[external ? 1 : 0];
            for (unsigned i = 0; i < 3; ++i) bias[i] = (*sum)[i] / samples;
            [NSUserDefaults.standardUserDefaults setObject:@[@(bias[0]), @(bias[1]), @(bias[2])]
                forKey:motionCalibrationKey(external)];
            dialog.message = @"Calibration complete.";
        } else {
            dialog.message = @"No steady motion samples. Close this message and try again.";
        }
    }];
}

- (Menu::Document)settingsDocument {
    Menu::Document document;
    document.CategorySidebar = true;
    document.BackClosesFromRows = true;
    std::array tabs{Menu::Tab{"General", {}}, Menu::Tab{"Controls", {}}, Menu::Tab{"Touch", {}},
                    Menu::Tab{"Graphics", {}}, Menu::Tab{"Audio", {}}, Menu::Tab{"HUD", {}},
                    Menu::Tab{"Quality of Life", {}}, Menu::Tab{"Models", {}}, Menu::Tab{"About", {}},
                    Menu::Tab{"Data", {}}};
    auto& [generalTab, controlsTab, touchTab, graphicsTab, audioTab, hudTab, qualityTab, modelsTab,
           aboutTab, dataTab] = tabs;
    const auto percent = [](int value) { return std::to_string(value) + "%"; };
    const auto multiplier = [](int value) {
        char text[24];
        std::snprintf(text, sizeof(text), "%.2f×", value / 100.f);
        return std::string(text);
    };
    const auto settingsChanged = [] { LoadGamePreferences(); };
    const auto stopped = [self] { return !self.live && !self.syncingDrive; };
    const auto heading = [](const char* name) {
        Menu::Row row;
        row.Kind = Menu::RowKind::Heading;
        row.Key = name;
        return row;
    };
    const auto action = [](const char* name, const char* help, std::function<void()> run) {
        Menu::Row row;
        row.Kind = Menu::RowKind::Action;
        row.Key = name;
        row.Help = help;
        row.Activate = [run] { run(); return Menu::WriteResult{}; };
        return row;
    };
    const auto info = [](const char* name, std::string value) {
        Menu::Row row;
        row.Kind = Menu::RowKind::Info;
        row.Key = name;
        row.Text = std::move(value);
        return row;
    };

    generalTab.Rows.push_back(heading("Appearance"));
    auto menuSize = PreferenceRow("MenuScale", "Menu size", 100, 50, 125, {}, multiplier);
    generalTab.Rows.push_back(std::move(menuSize));
    generalTab.Rows.push_back(action("Reset menu size", "", [] { SavePreference("MenuScale",100); }));

    controlsTab.Rows.push_back(heading("Game prompts"));
    controlsTab.Rows.push_back(PreferenceToggle("HideItemMenuPrompts", "Hide item menu button prompts", false,
                                             settingsChanged));
    controlsTab.Rows.push_back(PreferenceToggle("SingleScreenInstructions", "Single-screen game instructions", true,
                                             settingsChanged));
    controlsTab.Rows.push_back(PreferenceToggle("ControllerNeutralItemPrompts", "Controller neutral item prompts", true,
                                             settingsChanged));
    controlsTab.Rows.push_back(heading("Controller bindings"));
    for (const auto& binding : Controller::Bindings) {
        auto row = PreferenceRow(ControllerPreference(binding), binding.label, binding.defaultButton,
                                 0, Controller::Count - 1, settingsChanged);
        row.Kind = Menu::RowKind::Choice;
        row.Choices.assign(Controller::Names.begin(), Controller::Names.end());
        row.Help = "Assign a physical controller button. Home availability depends on the controller.";
        auto read = std::move(row.Read);
        row.Read = [read] {
            const auto buttons = Controller::ChoiceButtons(nintendoController(gameplayController()));
            return int(std::find(buttons.begin(), buttons.end(), read()) - buttons.begin());
        };
        auto write = std::move(row.Write);
        row.Write = [write](int index) {
            const auto buttons = Controller::ChoiceButtons(nintendoController(gameplayController()));
            if (index < 0 || index >= int(buttons.size()))
                return Menu::WriteResult{"Unknown controller button", true};
            return write(buttons[index]);
        };
        controlsTab.Rows.push_back(std::move(row));
    }
    controlsTab.Rows.push_back(action("Restore ReMasked controller layout",
        "Resets button assignments. Bumpers use items I and II, and triggers target and shield.", [] {
            for (const auto& binding : Controller::Bindings)
                SavePreference(ControllerPreference(binding).c_str(), binding.defaultButton);
            LoadGamePreferences();
        }));
    controlsTab.Rows.push_back(heading("Sticks and triggers"));
    controlsTab.Rows.push_back(PreferenceRow("MovementStickDeadZone", "Movement stick dead zone", 0, 0, 50, settingsChanged, percent));
    controlsTab.Rows.push_back(PreferenceRow("CameraStickDeadZone", "Camera stick dead zone", 0, 0, 50, settingsChanged, percent));
    controlsTab.Rows.push_back(PreferenceRow("TriggerDeadZone", "Trigger dead zone", 0, 0, 50, settingsChanged, percent));
    auto gyro = PreferenceRow("GyroSensitivity", "Gyro sensitivity", 100, 25, 400, settingsChanged, percent);
    gyro.Step = 5;
    controlsTab.Rows.push_back(std::move(gyro));
    controlsTab.Rows.push_back(action("Calibrate motion", "Keep the device or motion controller still.",
                                  [self] { [self calibrateMotion]; }));
    controlsTab.Rows.push_back(heading("Aiming"));
    auto aim = PreferenceToggle("StickOnly", "Aim source", false, settingsChanged);
    aim.Kind = Menu::RowKind::Choice;
    aim.Choices = {"Automatic (stick + motion)", "Right stick"};
    aim.Read = [] { return int(Preference("MotionSource", 1) == 0); };
    aim.Write = [settingsChanged](int index) {
        if (index == 1 && Preference("MotionSource", 1) != 0)
            SavePreference("MotionDevice", Preference("MotionSource", 1));
        SavePreference("MotionSource", index == 1 ? 0 : Preference("MotionDevice", 1));
        settingsChanged();
        return Menu::WriteResult{};
    };
    controlsTab.Rows.push_back(std::move(aim));
    auto motionDevice = PreferenceToggle("MotionDevice", "Motion device", false, settingsChanged);
    motionDevice.Kind = Menu::RowKind::Choice;
    motionDevice.Choices = {"Built-in device", "External controller"};
    motionDevice.Read = [] {
        const int source = Preference("MotionSource", 1);
        return (source ? source : Preference("MotionDevice", 1)) - 1;
    };
    motionDevice.Write = [settingsChanged](int index) {
        SavePreference("MotionDevice", index + 1);
        SavePreference("MotionSource", index + 1);
        settingsChanged();
        return Menu::WriteResult{};
    };
    motionDevice.Available = [] { return Preference("MotionSource", 1) != 0; };
    motionDevice.UnavailableReason = "Choose Automatic (stick + motion) first.";
    controlsTab.Rows.push_back(std::move(motionDevice));
    controlsTab.Rows.push_back(PreferenceToggle("InvertMotionX", "Invert aim X", false, settingsChanged));
    controlsTab.Rows.push_back(PreferenceToggle("InvertMotionY", "Invert aim Y", false, settingsChanged));
    touchTab.Rows.push_back(heading("Touch controls"));
    touchTab.Rows.push_back(PreferenceToggle("TouchVisible", "Show touch controls", true, settingsChanged));
    touchTab.Rows.push_back(PreferenceRow("TouchOpacity", "Touch-control opacity", 28, 0, 100, {}, percent));
    auto touchSize = PreferenceRow("TouchScale", "Touch-control size", 100, 50, 200, {}, percent);
    touchSize.Step = 10;
    touchTab.Rows.push_back(std::move(touchSize));
    touchTab.Rows.push_back(PreferenceToggle("TouchVibration", "Touch vibration", false, settingsChanged));
    touchTab.Rows.push_back(PreferenceToggle("HideWithController", "Hide with a controller", false));
    auto edit = action("Edit touch-control placement", "Move or resize controls for the current orientation.", [self] {
        [self.presentedViewController dismissViewControllerAnimated:NO completion:^{ [self editTouchLayout]; }];
    });
    edit.Available = [self] { return self.live && !airPlayDual; };
    edit.UnavailableReason = airPlayDual ? "Top Screen mode keeps the controls beside the 3DS screen."
        : "Start the game to edit the current touch layout.";
    touchTab.Rows.push_back(std::move(edit));
    touchTab.Rows.push_back(action("Reset layout", "Restore the default positions in all orientations.",
                                  [] { ReMaskedAzaharResetTouchLayout(); }));
    auto stick = PreferenceRow("FixedStick", "Movement stick position", 0, 0, 1);
    stick.Kind = Menu::RowKind::Choice;
    stick.Choices = {"Fixed", "Floating"};
    stick.Read = [] { return !Preference("FixedStick", 0); };
    stick.Write = [](int value) { SavePreference("FixedStick", !value); return Menu::WriteResult{}; };
    touchTab.Rows.push_back(std::move(stick));
    auto artwork = PreferenceToggle("LandscapeArtwork", "Equipped item artwork", true, settingsChanged);
    artwork.Help = "Draws equipped items, counts, action text and D-pad artwork on landscape touch controls.";
    touchTab.Rows.push_back(std::move(artwork));
    auto face = PreferenceRow("FaceLayout", "Face layout", 0, 0, 2);
    face.Kind = Menu::RowKind::Choice;
    face.Choices = {"Nintendo diamond", "Alternate diamond", "GameCube"};
    face.Help = "Changes the placement of the on-screen face buttons.";
    touchTab.Rows.push_back(std::move(face));
    auto cameraTouch = PreferenceRow("DragLook", "Camera touch mode", 0, 0, 1);
    cameraTouch.Kind = Menu::RowKind::Choice;
    cameraTouch.Choices = {"Stick", "Drag to look"};
    touchTab.Rows.push_back(std::move(cameraTouch));
    touchTab.Rows.push_back(PreferenceToggle("ShoulderLabels", "Shoulder labels", false));
    touchTab.Rows.push_back(heading("Individual controls"));
    for (unsigned i = 0; i < TouchControlNames.size(); ++i) {
        const auto label = std::string(TouchControlNames[i]);
        const auto key = std::string("Touch.") + std::to_string(i);
        touchTab.Rows.push_back(PreferenceToggle(key + ".Visible", "Show " + label, true));
        auto size = PreferenceRow(key + ".Size", label + " size", 100, 50, 200, {}, percent);
        size.Step = 5;
        touchTab.Rows.push_back(std::move(size));
    }

    graphicsTab.Rows.push_back(heading("Display"));
    auto picture = PreferenceRow("PictureAspect", "Aspect ratio", 0, 0, 3, [self] {
        [self.view setNeedsLayout];
        [self.view layoutIfNeeded];
    });
    picture.Kind = Menu::RowKind::Choice;
    picture.Choices = {"Original 3DS (5:3)", "16:9", "21:9", "Free"};
    picture.Help = "Choose the shape of the game picture. Free fills the display in landscape.";
    picture.Available = [] { return !Preference("SoftwareRenderer", 0); };
    picture.UnavailableReason = "Widescreen requires Metal via MoltenVK.";
    graphicsTab.Rows.push_back(std::move(picture));
    auto renderer = PreferenceRow("SoftwareRenderer", "Renderer", 0, 0, 1);
    renderer.Kind = Menu::RowKind::Choice;
    renderer.Choices = {"Metal via MoltenVK", "Software"};
    renderer.Available = stopped;
    renderer.UnavailableReason = "Change the renderer from startup Settings.";
    graphicsTab.Rows.push_back(std::move(renderer));
    graphicsTab.Rows.push_back(heading("Rendering"));
    const auto refreshGraphics = [self] { if (self.live) UpdateGraphicsOptions(); };
    auto resolution = PreferenceRow("Resolution", "Internal resolution", 0, 0, MaxInternalResolutionScale - 1, refreshGraphics);
    resolution.Kind = Menu::RowKind::Choice;
    for (int scale = 1; scale <= MaxInternalResolutionScale; ++scale)
        resolution.Choices.push_back(std::to_string(scale) + "× · " +
            std::to_string(400 * scale) + " × " + std::to_string(240 * scale));
    resolution.Available = [] { return !Preference("SoftwareRenderer", 0); };
    resolution.UnavailableReason = "Internal resolution requires Metal via MoltenVK.";
    graphicsTab.Rows.push_back(std::move(resolution));
    auto filter = PreferenceRow("TextureFilter", "Texture filter", 0, 0, 4, refreshGraphics);
    filter.Kind = Menu::RowKind::Choice;
    filter.Choices = {"None", "Bicubic", "ScaleForce", "xBRZ", "MMPX"};
    filter.Available = [] { return !Preference("SoftwareRenderer", 0); };
    filter.UnavailableReason = "Texture filtering requires Metal via MoltenVK.";
    graphicsTab.Rows.push_back(std::move(filter));
    auto shaders = PreferenceToggle("ShaderCache", "Remember compiled shaders", true);
    shaders.Available = [self] { return !self.live && !Preference("SoftwareRenderer", 0); };
    shaders.UnavailableReason = "Available from startup Settings with Metal via MoltenVK selected.";
    graphicsTab.Rows.push_back(std::move(shaders));
    graphicsTab.Rows.push_back(heading("Custom textures"));
    auto customTextures = PreferenceToggle("CustomTextures", "Use custom textures", true);
    customTextures.Available = stopped;
    customTextures.UnavailableReason = "Change custom textures from startup Settings.";
    graphicsTab.Rows.push_back(std::move(customTextures));
    graphicsTab.Rows.push_back(heading("Camera"));
    constexpr int defaultCameraDistance = 100;
    auto cameraDistance = PreferenceRow("CameraDistance", "Camera Distance (x)",
                                        defaultCameraDistance, 50, 300, settingsChanged, multiplier);
    cameraDistance.DefaultValue = defaultCameraDistance;
    graphicsTab.Rows.push_back(std::move(cameraDistance));
    graphicsTab.Rows.push_back(heading("Casting"));
    auto airPlay = PreferenceRow("AirPlayMode", "Cast layout", int(AirPlayMode::SingleScreen),
                                 int(AirPlayMode::SingleScreen), int(AirPlayMode::TopScreen),
                                 [self] { [self updateAirPlay]; });
    airPlay.Kind = Menu::RowKind::Choice;
    airPlay.Choices = {"AirPlay single screen gameplay", "AirPlay top screen"};
    airPlay.Help = "Connect from Screen Mirroring in Control Center. Single screen uses the iPhone as a touch controller. Top screen puts the touchable 3DS bottom screen between the iPhone’s controls.";
    graphicsTab.Rows.push_back(std::move(airPlay));

    audioTab.Rows.push_back(heading("Audio"));
    auto volume = PreferenceRow("Volume", "Volume", 100, 0, 100, [] {
        audioPlayer.volume = Preference("Volume", 100) / 100.f;
    }, percent);
    volume.Step = 5;
    volume.Available = [] { return !Preference("Muted", 0); };
    volume.UnavailableReason = "Turn Mute off first.";
    audioTab.Rows.push_back(std::move(volume));
    audioTab.Rows.push_back(PreferenceToggle("Muted", "Mute", false, [] {
        audioPlayer.volume = Preference("Muted", 0) ? 0.f : Preference("Volume", 100) / 100.f;
    }));
    audioTab.Rows.push_back(PreferenceToggle("MuteWhenUnfocused", "Mute when unfocused", false));
    audioTab.Rows.push_back(PreferenceToggle("MuteMusic", "Mute music", false, settingsChanged));

    hudTab.Rows.push_back(heading("Overall HUD"));
    hudTab.Rows.push_back(PreferenceRow("HudSize", "Overall HUD scale", 0, -10, 10, settingsChanged,
                                       [percent](int value) { return percent(100 + value * 5); }));
    hudTab.Rows.push_back(PreferenceRow("HudOpacity", "Overall HUD opacity", 100, 0, 100, settingsChanged, percent));
    using namespace MM3D::HudAdjustments;
    constexpr Part parts[]{Dpad, Magic, Hearts, Buttons, Clock, Rupees, Minigame, Carrots, Timer};
    const auto hudPercent = [percent](int value) { return percent(100 + value * 5); };
    const auto pixels = [](int value) { return std::to_string(value) + " px"; };
    hudTab.Rows.push_back(PreferenceRow("HudHeartSpacing", "Heart spacing", 0,
                                         -HeartSpacingLimit, HeartSpacingLimit, settingsChanged));
    hudTab.Rows.push_back(PreferenceRow("HudMagicHeight", "Magic meter height", 0, -10, 10,
                                         settingsChanged, hudPercent));
    for (Part part : parts) {
        hudTab.Rows.push_back(heading(Labels[part]));
        hudTab.Rows.push_back(PreferenceRow(HudPreference(part, "Size"), "Size", 0, -10, 10,
                                             settingsChanged, hudPercent));
        hudTab.Rows.push_back(PreferenceRow(HudPreference(part, "X"), "Horizontal offset", 0, -80, 80,
                                             settingsChanged, pixels));
        hudTab.Rows.push_back(PreferenceRow(HudPreference(part, "Y"), "Vertical offset", 0, -60, 60,
                                             settingsChanged, pixels));
    }
    hudTab.Rows.push_back(action("Reset gameplay HUD", "", [settingsChanged, parts] {
        for (Part part : parts)
            for (const char* property : {"Size", "X", "Y"})
                SavePreference(HudPreference(part, property).c_str(), 0);
        for (const char* key : {"HudSize", "HudMagicHeight", "HudHeartSpacing", "HideItemMenuPrompts"})
            SavePreference(key, 0);
        SavePreference("HudOpacity", 100);
        settingsChanged();
    }));
    hudTab.Rows.push_back(heading("Title screen"));
    auto titleOffset = [](int value) {
        char text[24];
        std::snprintf(text, sizeof(text), "%.2f px", value / 100.f);
        return std::string(text);
    };
    constexpr const char* poseNames[]{"Logo", "Copyright", "Press Start", "Loading icon"};
    for (unsigned i = 0; i < gameSettings.title.size(); ++i) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            const std::string key = "Title." + std::to_string(i) + "." + std::to_string(axis);
            const std::string label = std::string(poseNames[i]) +
                (axis == 0 ? " horizontal offset" : axis == 1 ? " vertical offset" : " size");
            const auto defaults = RemaskedGameSettings{}.title;
            auto row = PreferenceRow(key, label, std::lround(defaults[i][axis] * 100),
                                     axis == 2 ? 10 : -4000, axis == 2 ? 200 : 4000,
                                     settingsChanged, axis == 2 ? std::function<std::string(int)>(percent)
                                                                : std::function<std::string(int)>(titleOffset));
            hudTab.Rows.push_back(std::move(row));
        }
        const auto resetLabel = std::string("Reset ") + poseNames[i];
        hudTab.Rows.push_back(action(resetLabel.c_str(), "", [i, settingsChanged] {
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto key = "Title." + std::to_string(i) + "." + std::to_string(axis);
                [NSUserDefaults.standardUserDefaults removeObjectForKey:PreferenceKey(key.c_str())];
            }
            settingsChanged();
        }));
    }
    hudTab.Rows.push_back(action("Reset title screen", "", [] {
        for (unsigned pose = 0; pose < RemaskedGameSettings{}.title.size(); ++pose)
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto key = "Title." + std::to_string(pose) + "." + std::to_string(axis);
                [NSUserDefaults.standardUserDefaults removeObjectForKey:PreferenceKey(key.c_str())];
            }
        LoadGamePreferences();
    }));

    qualityTab.Rows.push_back(heading("Link movement"));
    qualityTab.Rows.push_back(PreferenceRow("WalkingSpeed", "Walk speed (x)", 100, 50, 300, settingsChanged, multiplier));
    qualityTab.Rows.push_back(PreferenceRow("ClimbingSpeed", "Climb speed (x)", 100, 50, 300, settingsChanged, multiplier));
    qualityTab.Rows.push_back(PreferenceRow("SwimmingSpeed", "Swim speed (x)", 100, 50, 300, settingsChanged, multiplier));
    qualityTab.Rows.push_back(action("Reset movement speeds", "", [settingsChanged] {
        for (const char* key : {"WalkingSpeed", "ClimbingSpeed", "SwimmingSpeed"}) SavePreference(key, 100);
        settingsChanged();
    }));
    qualityTab.Rows.push_back(PreferenceToggle("LowHealthBeepOff", "Disable low-heart beeping", false, settingsChanged));
    qualityTab.Rows.push_back(PreferenceToggle("TatlRemindersOff", "Disable Tatl reminders", false, settingsChanged));
    Menu::Tab restoration{"Restoration Fixes", {}};
    restoration.Rows.push_back(PreferenceToggle("BomberApproachOff", "Bombers don't approach automatically", false, settingsChanged));
    restoration.Rows.push_back(PreferenceToggle("SwordActivateOwls", "Activate owl statues with a sword", false, settingsChanged));
    restoration.Rows.push_back(PreferenceToggle("ClassicDoubleTime", "Classic Song of Double Time", false, settingsChanged));
    auto classicSaving = PreferenceToggle("ClassicSaving", "Classic saving", false, settingsChanged);
    classicSaving.Available = stopped;
    classicSaving.UnavailableReason = "Change this from startup Settings.";
    classicSaving.Help = "Use the Song of Time to establish a permanent save. Owl statues then provide a one-use local save.";
    restoration.Rows.push_back(std::move(classicSaving));
    Menu::Tab difficulty{"Difficulty", {}};
    difficulty.Rows.push_back(PreferenceRow("DamageMultiplier", "Damage taken", 1, 1, 16,
        settingsChanged, [](int value) { return std::to_string(value) + "×"; }));
    difficulty.Rows.push_back(PreferenceRow("RedPotionHearts", "Red potion healing", 20, 1, 20, settingsChanged));
    difficulty.Rows.push_back(PreferenceRow("BluePotionHearts", "Blue potion healing", 20, 1, 20, settingsChanged));
    difficulty.Rows.push_back(PreferenceRow("BottledFairyHearts", "Bottled fairy healing", 10, 1, 20, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("WeakerEnhancedDefense", "Weaker enhanced defense", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("HeartDropsOff", "Disable heart drops", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("RupeeDropsOff", "Disable rupee drops", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("FairyDropsOff", "Disable fairy drops", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("MagicDropsOff", "Disable magic drops", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("AmmoPickupsOff", "Disable ammo pickups", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("LoseRupeesOnDeath", "Lose rupees on death", false, settingsChanged));
    auto deathTime = PreferenceToggle("AdvanceTimeOnDeath", "Dying moves time forward 6 hours", false, settingsChanged);
    deathTime.Help = "Ending the world takes priority when both options are on.";
    difficulty.Rows.push_back(std::move(deathTime));
    difficulty.Rows.push_back(PreferenceToggle("FastTime", "Fast time", false, settingsChanged));
    difficulty.Rows.push_back(PreferenceToggle("EndWorldOnDeath", "World ends on death", false, settingsChanged));
    auto doublePrices = PreferenceToggle("DoubleShopPrices", "Increase purchase prices and fees", false, settingsChanged);
    doublePrices.Available = stopped;
    doublePrices.UnavailableReason = "Change this from startup Settings.";
    doublePrices.Help = "Applies when you next start the game.";
    difficulty.Rows.push_back(std::move(doublePrices));
    auto resetDifficulty = action("Reset difficulty", "", [settingsChanged] {
        for (const char* key : {"WeakerEnhancedDefense", "HeartDropsOff", "RupeeDropsOff",
                "FairyDropsOff", "MagicDropsOff", "AmmoPickupsOff", "LoseRupeesOnDeath",
                "EndWorldOnDeath", "AdvanceTimeOnDeath", "FastTime"}) SavePreference(key, 0);
        SavePreference("DoubleShopPrices", 0);
        SavePreference("DamageMultiplier", 1);
        SavePreference("RedPotionHearts", 20);
        SavePreference("BluePotionHearts", 20);
        SavePreference("BottledFairyHearts", 10);
        settingsChanged();
    });
    resetDifficulty.Available = stopped;
    resetDifficulty.UnavailableReason = "Reset difficulty from startup Settings.";
    difficulty.Rows.push_back(std::move(resetDifficulty));

    auto model = action("Import prepared player model", "Choose a prepared MM3D actor or folder from Files.",
                        [self] { [self choosePreparedModel]; });
    model.Available = stopped;
    model.UnavailableReason = "Change models from startup Settings.";
    modelsTab.Rows.push_back(std::move(model));
    auto originalModel = action("Use original Link", "Deactivate the actor installed through this menu.",
                                [self] { [self restoreOriginalModel]; });
    originalModel.Available = stopped;
    originalModel.UnavailableReason = "Change models from startup Settings.";
    modelsTab.Rows.push_back(std::move(originalModel));
    NSBundle* appBundle = NSBundle.mainBundle;
    NSString* version = [appBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    NSString* build = [appBundle objectForInfoDictionaryKey:@"CFBundleVersion"];
    if (version.length) aboutTab.Rows.push_back(info("Version", version.UTF8String));
    if (build.length) aboutTab.Rows.push_back(info("Build", build.UTF8String));
    using ReMaskedDrive::Content;
    using ReMaskedDrive::Resolution;
    auto folder = info("Current data folder", "");
    folder.DynamicText = [] {
        return Preference("ICloudDriveEnabled", 1) ? "iCloud Drive / ReMasked Azahar" : "On this iPhone";
    };
    dataTab.Rows.push_back(std::move(folder));
    auto syncStatus = info("Folder sync", "");
    syncStatus.DynamicText = [] {
        return Preference("ICloudDriveEnabled", 1) ? std::string(ReMaskedDrive::Status().UTF8String) : "Using local data";
    };
    dataTab.Rows.push_back(std::move(syncStatus));
    const auto driveReady = [self] { return !self.live && !self.syncingDrive; };
    auto drive = PreferenceToggle("ICloudDriveEnabled", "Use iCloud Drive", true, [self] {
        if (Preference("ICloudDriveEnabled", 1)) [self prepareDriveSaves];
    });
    drive.Help = "ReMasked creates and manages its own folder. Gameplay uses local working copies.";
    drive.Available = driveReady;
    drive.UnavailableReason = "Available from startup Settings.";
    dataTab.Rows.push_back(std::move(drive));
    auto sync = action("Sync data folder now", "Syncs saves, textures and prepared models. ReMasked checks for newer saves when you reopen the app.",
                       [self] { [self syncDrive:Content::AzaharData resolution:Resolution::Ask showResult:YES]; });
    sync.Available = [driveReady] { return driveReady() && Preference("ICloudDriveEnabled", 1); };
    sync.UnavailableReason = "Enable iCloud Drive from startup Settings.";
    dataTab.Rows.push_back(std::move(sync));
    auto rom = action("Choose ROM", "Select your local game file in Files.", [self] { [self chooseROM]; });
    rom.Available = stopped;
    rom.UnavailableReason = "Choose the ROM from startup Settings.";
    dataTab.Rows.push_back(std::move(rom));
    auto import = action("Import saves", "Select save0.bin, save1.bin or save2.bin.", [self] { [self importSaves]; });
    import.Available = stopped;
    import.UnavailableReason = "Import saves from startup Settings.";
    dataTab.Rows.insert(dataTab.Rows.begin(), std::move(import));
    auto exportSaves = action("Export saves", "Copy save slots to a folder in Files.", [self] { [self exportSaves]; });
    exportSaves.Available = stopped;
    exportSaves.UnavailableReason = "Export saves from startup Settings.";
    dataTab.Rows.insert(dataTab.Rows.begin() + 1, std::move(exportSaves));
    Menu::Row icons;
    icons.Kind = Menu::RowKind::Choice;
    icons.Key = "App icon";
    icons.Choices = {"Combined", "Icon 1", "Icon 2", "Icon 3", "Icon 4"};
    for (int i = 0; i < int(icons.Choices.size()); ++i)
        icons.ChoiceImages.push_back("ReMaskedIcon" + std::to_string(i) + ".png");
    icons.Read = [] {
        NSString* name = UIApplication.sharedApplication.alternateIconName;
        for (int index = 1; index <= 4; ++index)
            if ([name isEqualToString:[NSString stringWithFormat:@"AppIcon%d", index]]) return index;
        return 0;
    };
    icons.Available = [] { return bool(UIApplication.sharedApplication.supportsAlternateIcons); };
    icons.UnavailableReason = "App icon changes are unavailable on this device.";
    icons.Write = [self](int index) {
        UIApplication* app = UIApplication.sharedApplication;
        NSString* name = index ? [NSString stringWithFormat:@"AppIcon%d",index] : nil;
        if ((name == nil && app.alternateIconName == nil) || [name isEqualToString:app.alternateIconName])
            return Menu::WriteResult{};
        [app setAlternateIconName:name completionHandler:^(NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                UIViewController* presented = self.presentedViewController;
                if (![presented isKindOfClass:ReMaskedSettingsPanel.class]) return;
                [(ReMaskedSettingsPanel*)presented refreshSettings];
                if (error) {
                    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"App Icon"
                        message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
                    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
                    [presented presentViewController:alert animated:YES completion:nil];
                }
            });
        }];
        return Menu::WriteResult{};
    };
    generalTab.Rows.push_back(std::move(icons));
    generalTab.Rows.push_back(heading("Language"));
    auto language = PreferenceRow("GameLanguage", "Game language", 0, 0, int(GameLanguages.size()) - 1);
    language.Kind = Menu::RowKind::Choice;
    language.Choices.assign(GameLanguages.begin(), GameLanguages.end());
    language.Help = "Applies at the next game launch. Available translations depend on your ROM.";
    language.Available = stopped;
    language.UnavailableReason = "Change the game language from startup Settings.";
    generalTab.Rows.push_back(std::move(language));
    namespace Cheats = MM3D::SimpleCheats;
    Menu::Tab cheatsTab{"Cheats", {}};
    const auto cheatState = [] {
        std::array<int, 4> state{};
        remasked_get_cheats(state.data());
        return state;
    };
    const auto cheatAvailable = [self, cheatState] { return self.live && cheatState()[0] != 0; };
    auto cheatStatus = info("Cheat status", "");
    cheatStatus.DynamicText = [cheatAvailable, cheatState] {
        if (!cheatAvailable()) return "Load a supported save to use cheats.";
        const auto state = cheatState();
        if (state[2]) return "Cheat queued. Resume the game to apply.";
        if (state[3] > 0) return "Cheat applied.";
        if (state[3] < 0) return "Cheat could not be applied.";
        return "One-time cheats apply when you resume. Inventory changes can be saved with the game.";
    };
    cheatsTab.Rows.push_back(std::move(cheatStatus));
    cheatsTab.Rows.push_back(heading("One-time cheats"));
    for (unsigned i = 0; i < Cheats::Count; ++i) {
        Menu::Row row;
        row.Kind = Menu::RowKind::Action;
        row.Key = Cheats::Labels[i];
        row.Help = Cheats::Help[i];
        row.Activate = [i] {
            return remasked_queue_cheat(i) ? Menu::WriteResult{} :
                Menu::WriteResult{"Load a supported save to use this cheat.", true};
        };
        row.Available = cheatAvailable;
        row.UnavailableReason = "Load a supported save to use this cheat.";
        cheatsTab.Rows.push_back(std::move(row));
    }
    cheatsTab.Rows.push_back(heading("Infinite resources"));
    for (unsigned i = 0; i < Cheats::Ammunition.size() + 2; ++i) {
        Menu::Row row;
        const int bit = 1 << i;
        const auto enabled = [cheatState, bit] { return (cheatState()[1] & bit) != 0; };
        row.Kind = Menu::RowKind::Toggle;
        row.Key = i < Cheats::Ammunition.size() ? Cheats::Ammunition[i].label :
            i == Cheats::Ammunition.size() ? "Infinite health" : "Infinite magic";
        row.Help = "Active for this game session.";
        row.Available = [cheatAvailable, enabled] { return cheatAvailable() || enabled(); };
        row.UnavailableReason = "Load a supported save to use this cheat.";
        row.Read = enabled;
        row.Write = [i](int on) {
            return remasked_set_continuous_cheat(i, on != 0) ? Menu::WriteResult{} :
                Menu::WriteResult{"Load a supported save to use this cheat.", true};
        };
        cheatsTab.Rows.push_back(std::move(row));
    }
    cheatsTab.Rows.push_back(action("Turn off all infinite cheats", "", [] {
        remasked_set_continuous_cheat(-1, false);
    }));
    for (auto* tab : {&generalTab, &controlsTab, &touchTab, &graphicsTab, &modelsTab, &audioTab,
                      &hudTab, &restoration, &qualityTab, &cheatsTab, &difficulty, &dataTab, &aboutTab})
        document.Tabs.push_back(std::move(*tab));
    return document;
}

- (void)mark {
    NSLog(@"REMASKED_AZAHAR_MARK frames=%lu hitches=%lu maxMs=%.1f",(unsigned long)self.frames,(unsigned long)self.hitches,self.worst);
    self.frames = self.hitches = 0; self.worst = 0;
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    if (view == airPlayScreen) [self updateTouchPicture];
}
- (void)drawInMTKView:(MTKView*)view {
    if (!self.live || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) { self.last = 0; return; }
    const double now = CACurrentMediaTime();
    GCController* controller = gameplayController();
    GCExtendedGamepad* pad = controller.extendedGamepad;
    const bool settingsDown = sampleFrameInput(controller, pad);
    const bool openSettings = settingsDown && !self.settingsButtonHeld;
    self.settingsButtonHeld = settingsDown;
    if (openSettings) { [self openSettings]; return; }
    const double startRun = CACurrentMediaTime();
    const double gap = self.last ? (now-self.last)*1000 : 0;
    self.last = now;
    self.frames++;
    if (gap > 50) self.hitches++;
    self.worst = std::max(self.worst,gap);
    retro_run();
    if (self.frames == 1) NSLog(@"REMASKED_AZAHAR_FIRST_TICK");
    const double endRun = CACurrentMediaTime();
    if (video.present(view, airPlayScreen, airPlayDual)) [self updateTouchPicture];
    const double inputMs = (startRun-now)*1000, coreMs = (endRun-startRun)*1000;
    const double videoMs = (CACurrentMediaTime()-endRun)*1000;
    if (gap >= 100 || coreMs >= 100 || videoMs >= 100)
        NSLog(@"REMASKED_AZAHAR_HITCH frame=%lu gapMs=%.1f inputMs=%.1f coreMs=%.1f videoMs=%.1f",
              (unsigned long)self.frames,gap,inputMs,coreMs,videoMs);

    static const bool captureTiming = getenv("REMASKED_CONTROLLER_TIMING") != nullptr;
    if (captureTiming) {
        static unsigned frames, slowFrames;
        static double totalGap, totalInput, totalCore, totalVideo;
        static bool hadController, hadAirPlay, hadDual;
        const bool connected = pad != nil, external = airPlayScreen != nil;
        if (connected != hadController || external != hadAirPlay || airPlayDual != hadDual) {
            NSLog(@"REMASKED_FRAME_MODE controller=%d airPlay=%d dual=%d", connected, external, airPlayDual);
            hadController = connected; hadAirPlay = external; hadDual = airPlayDual;
            frames = slowFrames = 0;
            totalGap = totalInput = totalCore = totalVideo = 0;
        }
        if (gap > 0) {
            totalGap += gap; totalInput += inputMs; totalCore += coreMs; totalVideo += videoMs;
            slowFrames += gap > 33.4;
            if (++frames == 120) {
                NSLog(@"REMASKED_FRAME_TIMING controller=%d airPlay=%d dual=%d fps=%.1f slow=%u inputMs=%.2f coreMs=%.2f videoMs=%.2f",
                      connected, external, airPlayDual, 1000*frames/totalGap, slowFrames,
                      totalInput/frames,totalCore/frames,totalVideo/frames);
                frames = slowFrames = 0;
                totalGap = totalInput = totalCore = totalVideo = 0;
            }
        }
    }
}
@end

@interface ReMaskedAirPlayScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow* window;
@property(nonatomic,strong) MTKView* videoView;
@end

@implementation ReMaskedAirPlayScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
      options:(UISceneConnectionOptions*)options {
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    UIViewController* controller = [UIViewController new];
    self.videoView = [[MTKView alloc] initWithFrame:self.window.bounds device:MTLCreateSystemDefaultDevice()];
    self.videoView.paused = YES;
    self.videoView.framebufferOnly = YES;
    self.videoView.clearColor = MTLClearColorMake(0,0,0,1);
    self.videoView.backgroundColor = UIColor.blackColor;
    controller.view = self.videoView;
    self.window.rootViewController = controller;
    self.window.hidden = NO;
    airPlayScreen = self.videoView;
    [airPlayHost updateAirPlay];
}
- (void)sceneDidDisconnect:(UIScene*)scene {
    if (airPlayScreen == self.videoView) {
        airPlayScreen = nil;
        [airPlayHost updateAirPlay];
    }
    self.window = nil;
    self.videoView = nil;
}
@end

@interface ReMaskedScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow* window;
@property(nonatomic,strong) id airPlayRegistration;
@property(nonatomic) BOOL returningFromBackground;
@end
@implementation ReMaskedScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
      options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    ReMaskedPrototype* host = [ReMaskedPrototype new];
    self.window.rootViewController = host;
    [self.window makeKeyAndVisible];
    airPlayHost = host;
    if (@available(iOS 27.0, *)) {
        // The same public iOS 27 API is available at runtime when built with an older SDK.
        Class accessoryClass = NSClassFromString(@"UISceneAccessory");
        SEL makeAccessory = NSSelectorFromString(@"externalNonInteractiveSceneAccessoryWithConfiguration:");
        SEL registerAccessory = NSSelectorFromString(@"registerSceneAccessory:");
        if ([accessoryClass respondsToSelector:makeAccessory] && [host respondsToSelector:registerAccessory]) {
            UISceneConfiguration* configuration = [[UISceneConfiguration alloc]
                initWithName:@"ReMasked AirPlay" sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
            configuration.delegateClass = ReMaskedAirPlayScene.class;
            auto sendObject = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend);
            id accessory = sendObject(accessoryClass, makeAccessory, configuration);
            if (accessory)
                self.airPlayRegistration = sendObject(host, registerAccessory, accessory);
        }
    }
    [host updateAirPlay];
    [host prepareDriveSaves];
    NSLog(@"REMASKED_AZAHAR_LAUNCHER");
    if (getenv("REMASKED_AZAHAR_SMOKE")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            [host startGame];
        });
    }
}
- (void)sceneDidEnterBackground:(UIScene*)scene {
    self.returningFromBackground = YES;
    [(ReMaskedPrototype*)self.window.rootViewController backUpDriveSaves];
}
- (void)sceneWillResignActive:(UIScene*)scene {
    ReMaskedPrototype* host = (ReMaskedPrototype*)self.window.rootViewController;
    [host.bottomTouchView cancelTouch];
    ReMaskedAzaharReleaseTouch();
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    if (airPlayScreen || Preference("MuteWhenUnfocused", 0)) [audioPlayer stop];
}
- (void)sceneDidBecomeActive:(UIScene*)scene {
    ReMaskedPrototype* host = (ReMaskedPrototype*)self.window.rootViewController;
    [host updateAirPlayIdleTimer];
    if (host.live && !audioPlayer.playing) [audioPlayer play];
    if (self.returningFromBackground || host.foregroundSaveCheckPending) {
        self.returningFromBackground = NO;
        [host refreshSavesAfterForeground];
    }
}
@end
@interface ReMaskedApp : UIResponder <UIApplicationDelegate>
@end
@implementation ReMaskedApp
@end
int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(ReMaskedApp.class)); }
}
