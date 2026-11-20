#if defined(REMASKED_ANDROID_MENU)
#include "fast/extension/remasked_interface_fonts.h"
#include REMASKED_INTERFACE_FONTS_HEADER
#include <cstdlib>
#include <algorithm>
#endif
#define NOMINMAX

#include "ship/window/gui/Gui.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

#include "ship/config/Config.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/resource/File.h"
#include <stb_image.h>
#include "ship/window/gui/Fonts.h"
#include "ship/window/gui/resource/GuiTextureFactory.h"
#include "ship/window/gui/resource/GuiTexture.h"

namespace Ship {
#define TOGGLE_BTN ImGuiKey_F1
#define TOGGLE_PAD_BTN ImGuiKey_GamepadBack

Gui::Gui(std::vector<std::shared_ptr<GuiWindow>> guiWindows) : mNeedsConsoleVariableSave(false) {
    mGameOverlay = std::make_shared<GameOverlay>();

    for (auto& guiWindow : guiWindows) {
        AddGuiWindow(guiWindow);
    }

    // Add default windows if we don't already have one by the name
    if (GetGuiWindow("Stats") == nullptr) {
        AddGuiWindow(std::make_shared<StatsWindow>(CVAR_STATS_WINDOW_OPEN, "Stats"));
    }

    if (GetGuiWindow("SDLAddRemoveDeviceEventHandler") == nullptr) {
        AddGuiWindow(std::make_shared<SDLAddRemoveDeviceEventHandler>("gOpenWindows.SDLAddRemoveDeviceEventHandler",
                                                                      "SDLAddRemoveDeviceEventHandler"));
    }

    if (GetGuiWindow("Console") == nullptr) {
        AddGuiWindow(std::make_shared<ConsoleWindow>(CVAR_CONSOLE_WINDOW_OPEN, "Console", ImVec2(520, 600),
                                                     ImGuiWindowFlags_NoFocusOnAppearing));
    }
}

Gui::Gui() : Gui(std::vector<std::shared_ptr<GuiWindow>>()) {
}

Gui::~Gui() {
    SPDLOG_TRACE("destruct gui");
}

void Gui::Init() {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    mImGuiIo = &ImGui::GetIO();
    mImGuiIo->ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NoMouseCursorChange;

    // Add Font Awesome and merge it into the default font.
    #if defined(REMASKED_ANDROID_MENU)
    const char* densityText=std::getenv("REMASKED_ANDROID_DENSITY");
    const float density=densityText?std::clamp(std::strtof(densityText,nullptr),1.f,4.f):1.f;
    // Bake near the displayed pixel size: a giant atlas minified several
    // times loses thin strokes because the font texture has no mip chain.
    ImFontConfig interfaceFont;interfaceFont.FontDataOwnedByAtlas=false;interfaceFont.OversampleH=2;interfaceFont.OversampleV=2;
    for(size_t i=0;i<ReMaskedSettings::SizedFaces.size();++i){
        const float pixels=ReMaskedSettings::InterfacePixelSizes[i];
        auto body=mImGuiIo->Fonts->AddFontFromMemoryTTF(ReMaskedSettings::FontData::Body,sizeof(ReMaskedSettings::FontData::Body),pixels,&interfaceFont,ReMaskedSettings::GlyphRanges);
        auto heading=mImGuiIo->Fonts->AddFontFromMemoryTTF(ReMaskedSettings::FontData::Heading,sizeof(ReMaskedSettings::FontData::Heading),pixels,&interfaceFont,ReMaskedSettings::GlyphRanges);
        ReMaskedSettings::SizedFaces[i]={body,heading,pixels};
    }
    const auto defaults=ReMaskedSettings::FacesForSize(24.f);
    ReMaskedSettings::BodyFont=defaults.Body;ReMaskedSettings::HeadingFont=defaults.Heading;ReMaskedSettings::FontPixels=defaults.Pixels;
    ReMaskedSettings::BoldFont=mImGuiIo->Fonts->AddFontFromMemoryTTF(ReMaskedSettings::FontData::Bold,sizeof(ReMaskedSettings::FontData::Bold),ReMaskedSettings::FontPixels,&interfaceFont,ReMaskedSettings::GlyphRanges);
    mImGuiIo->FontDefault=ReMaskedSettings::BodyFont;
#else
    mImGuiIo->Fonts->AddFontDefault();
#endif
    // This must match the default font size, which is 13.0f.
    #if defined(REMASKED_ANDROID_MENU)
    float baseFontSize = ReMaskedSettings::FontPixels;
#else
    float baseFontSize = 13.0f;
#endif
    // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly
    float iconFontSize = baseFontSize * 2.0f / 3.0f;
    static const ImWchar sIconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
#if defined(REMASKED_ANDROID_MENU)
    iconsConfig.DstFont=ReMaskedSettings::BodyFont;
#endif
    iconsConfig.PixelSnapH = true;
    iconsConfig.GlyphMinAdvanceX = iconFontSize;
    mImGuiIo->Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, iconFontSize,
                                                          &iconsConfig, sIconsRanges);

#if defined(REMASKED_ANDROID_MENU)
    ImGui::GetStyle().ScaleAllSizes(density);
    mImGuiIo->FontGlobalScale=15.f*density/ReMaskedSettings::FontPixels;
#elif defined(__ANDROID__)
    // Scale everything by 2 for Android
    ImGui::GetStyle().ScaleAllSizes(2.0f);
    mImGuiIo->FontGlobalScale = 2.0f;
#endif

    // An installation keeps what it writes in data/, not beside the executable.
    mImGuiIniPath = Context::GetPathRelativeToAppDirectory("imgui.ini");
    if (std::error_code error; std::filesystem::is_directory(Context::GetPathRelativeToAppDirectory("data"), error))
        mImGuiIniPath = Context::GetPathRelativeToAppDirectory("data/imgui.ini");
    mImGuiLogPath = Context::GetPathRelativeToAppDirectory("imgui_log.txt");
    mImGuiIo->IniFilename = mImGuiIniPath.c_str();
    mImGuiIo->LogFilename = mImGuiLogPath.c_str();

    if (SupportsViewports() &&
        Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_ENABLE_MULTI_VIEWPORTS, 1)) {
        mImGuiIo->ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }

    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0) &&
        GetMenuOrMenubarVisible()) {
        mImGuiIo->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    } else {
        mImGuiIo->ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }

    GetGuiWindow("Stats")->Init();
    GetGuiWindow("Console")->Init();
    GetGameOverlay()->Init();

    Context::GetRawInstance()->GetResourceManager()->GetResourceLoader()->RegisterResourceFactory(
        std::make_shared<ResourceFactoryBinaryGuiTextureV0>(), RESOURCE_FORMAT_BINARY, "GuiTexture",
        static_cast<uint32_t>(RESOURCE_TYPE_GUI_TEXTURE), 0);

    ImGuiWMInit();
    ImGuiBackendInit();
}

void Gui::ImGuiWMInit() {
}

void Gui::ShutDownImGui(Ship::Window* window) {
    // A window whose backend never came up (no display, SDL video init failure)
    // is destroyed before Init() created the ImGui context; DestroyContext()
    // dereferences the missing context, so there is nothing to shut down.
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiWMShutdown();
    ImGuiBackendShutdown();
    ImGui::DestroyContext();
}

void Gui::ImGuiWMShutdown() {
}

void Gui::ImGuiBackendInit() {
}

void Gui::ImGuiBackendShutdown() {
}

bool Gui::SupportsViewports() {
    return false;
}

bool Gui::GamepadNavigationEnabled() {
    return mImGuiIo->ConfigFlags & ImGuiConfigFlags_NavEnableGamepad;
}

void Gui::BlockGamepadNavigation() {
    mImGuiIo->ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
}

void Gui::UnblockGamepadNavigation() {
    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0) &&
        GetMenuOrMenubarVisible()) {
        mImGuiIo->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
}

ImGuiID Gui::GetMainGameWindowID() {
    static ImGuiID windowID = 0;
    if (windowID != 0) {
        return windowID;
    }
    ImGuiWindow* window = ImGui::FindWindowByName("Main Game");
    if (window == NULL) {
        return 0;
    }
    windowID = window->ID;
    return windowID;
}

void Gui::ImGuiBackendNewFrame() {
}

void Gui::ImGuiWMNewFrame() {
}

void Gui::DrawMenu() {
    const std::shared_ptr<Window> wnd = Context::GetRawInstance()->GetWindow();
    const std::shared_ptr<Config> conf = Context::GetRawInstance()->GetConfig();

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                   ImGuiWindowFlags_NoResize;

    if (GetMenuBar() && GetMenuBar()->IsVisible()) {
        windowFlags |= ImGuiWindowFlags_MenuBar;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2((int)wnd->GetWidth(), (int)wnd->GetHeight()));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::Begin("Main - Deck", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    mTemporaryWindowPos = ImGui::GetWindowPos();

    const ImGuiID dockId = ImGui::GetID("main_dock");

    if (!ImGui::DockBuilderGetNode(dockId)) {
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_NoTabBar);
        ImGui::DockBuilderSetNodeSize(dockId, ImVec2(viewport->Size.x, viewport->Size.y));

        ImGui::DockBuilderDockWindow("Main Game", dockId);

        ImGui::DockBuilderFinish(dockId);
    }

    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None | ImGuiDockNodeFlags_NoDockingInCentralNode);

    if (ImGui::IsKeyPressed(TOGGLE_BTN, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        (ImGui::IsKeyPressed(TOGGLE_PAD_BTN, false) &&
         Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0))) {
        if ((ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(TOGGLE_PAD_BTN, false)) && GetMenu()) {
            GetMenu()->ToggleVisibility();
        } else if ((ImGui::IsKeyPressed(TOGGLE_BTN, false) || ImGui::IsKeyPressed(TOGGLE_PAD_BTN, false)) &&
                   GetMenuBar()) {
            GetMenuBar()->ToggleVisibility();
        }
        Ship::Context::GetRawInstance()->GetWindow()->GetMouseStateManager()->UpdateMouseCapture();
        if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0) &&
            GetMenuOrMenubarVisible()) {
            mImGuiIo->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        } else {
            mImGuiIo->ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        }
    }

    // Mac interprets this as cmd+r when io.ConfigMacOSXBehavior is on (on by default)
    if ((ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) &&
        ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        std::reinterpret_pointer_cast<ConsoleWindow>(
            Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
            ->Dispatch("reset");
    }

    if (GetMenuBar()) {
        GetMenuBar()->Update();
        GetMenuBar()->Draw();
    }

    if (GetMenu()) {
        GetMenu()->Update();
        GetMenu()->Draw();
    }

    for (auto& windowIter : mGuiWindows) {
        windowIter.second->Update();
        windowIter.second->Draw();
    }

    ImGui::End();
}

void Gui::HandleMouseCapture() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMouseInputs;
    for (auto windowIter : ImGui::GetCurrentContext()->WindowsById.Data) {
        if (windowIter.key != GetMainGameWindowID() && windowIter.key != GetGameOverlay()->GetID()) {
            ImGuiWindow* window = (ImGuiWindow*)windowIter.val_p;
            if (Context::GetRawInstance()->GetWindow()->IsMouseCaptured()) {
                window->Flags |= flags;
            } else {
                window->Flags &= ~(flags);
            }
        }
    }
}

void Gui::StartFrame() {
    HandleMouseCapture();
    ImGuiBackendNewFrame();
    ImGuiWMNewFrame();
    ImGui::NewFrame();
}

void Gui::EndFrame() {
    // Draw the ImGui "viewports" which are the floating windows.
    ImGui::Render();
    ImGuiRenderDrawData(ImGui::GetDrawData());
    ImGui::EndFrame();
}

void Gui::CalculateGameViewport() {
}

void Gui::DrawGame() {
}

void Gui::DrawFloatingWindows() {
}

void Gui::CheckSaveCvars() {
    if (mNeedsConsoleVariableSave) {
        Ship::Context::GetRawInstance()->GetConsoleVariables()->Save();
        mNeedsConsoleVariableSave = false;
    }
}

void Gui::StartDraw() {
    // Initialize the frame.
    StartFrame();
    // Draw the gui menus
    DrawMenu();
    // Calculate the available space the game can render to
    CalculateGameViewport();
}

void Gui::EndDraw() {
    // Draw the game framebuffer into ImGui
    DrawGame();
    // End the frame
    EndFrame();
    // Draw the ImGui floating windows.
    DrawFloatingWindows();
    // Check if the CVars need to be saved, and do it if so.
    CheckSaveCvars();
}

void Gui::ImGuiRenderDrawData(ImDrawData* data) {
}

void Gui::SaveConsoleVariablesNextFrame() {
    mNeedsConsoleVariableSave = true;
}

void Gui::AddGuiWindow(std::shared_ptr<GuiWindow> guiWindow) {
    if (mGuiWindows.contains(guiWindow->GetName())) {
        SPDLOG_ERROR("ImGui::AddGuiWindow: Attempting to add duplicate window name {}", guiWindow->GetName());
        return;
    }

    mGuiWindows[guiWindow->GetName()] = guiWindow;
    guiWindow->Init();
}

void Gui::RemoveGuiWindow(std::shared_ptr<GuiWindow> guiWindow) {
    RemoveGuiWindow(guiWindow->GetName());
}

void Gui::RemoveGuiWindow(const std::string& name) {
    mGuiWindows.erase(name);
}

void Ship::Gui::RemoveAllGuiWindows() {
    mGuiWindows.clear();
}

std::shared_ptr<GuiWindow> Gui::GetGuiWindow(const std::string& name) {
    if (mGuiWindows.contains(name)) {
        return mGuiWindows[name];
    } else {
        return nullptr;
    }
}

std::shared_ptr<GameOverlay> Gui::GetGameOverlay() {
    return mGameOverlay;
}

void Gui::SetMenuBar(std::shared_ptr<GuiMenuBar> menuBar) {
    mMenuBar = menuBar;

    if (GetMenuBar()) {
        GetMenuBar()->Init();
    }
}

void Gui::SetMenu(std::shared_ptr<GuiWindow> menu) {
    mMenu = menu;

    if (GetMenu()) {
        GetMenu()->Init();
    }
}

std::shared_ptr<GuiMenuBar> Gui::GetMenuBar() {
    return mMenuBar;
}

bool Gui::GetMenuOrMenubarVisible() {
    return (GetMenuBar() && GetMenuBar()->IsVisible()) || (GetMenu() && GetMenu()->IsVisible());
}

bool Gui::GetAnyGuiWindowVisible() {
    return std::any_of(mGuiWindows.begin(), mGuiWindows.end(), [](const auto& entry) {
        return entry.second != nullptr && entry.second->IsVisible();
    });
}

bool Gui::IsMouseOverAnyGuiItem() {
    return ImGui::IsAnyItemHovered();
}

bool Gui::IsMouseOverActivePopup() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx->OpenPopupStack.Size == 0 || ctx->HoveredWindow == NULL) {
        return false;
    }
    ImGuiPopupData data = ctx->OpenPopupStack.back();
    if (data.Window == NULL) {
        return false;
    }
    return (ctx->HoveredWindow->ID == data.Window->ID);
}

std::shared_ptr<GuiWindow> Gui::GetMenu() {
    return mMenu;
}
} // namespace Ship
