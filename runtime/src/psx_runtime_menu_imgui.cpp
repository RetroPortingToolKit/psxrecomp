#include "psx_runtime_menu.h"
#if defined(RECOMP_LAUNCHER) && !defined(PSX_SDL_NO_RENDER)
#include "psx_sdl.h"
#include "recomp_runtime_ui.h"
#include "common/launcher_theme.h"
#include "imgui.h"
#include <filesystem>
#include "backends/imgui_impl_opengl3.h"
#if defined(PSX_SDL3)
#include "backends/imgui_impl_sdl3.h"
#define UI_SDL(name) ImGui_ImplSDL3_##name
#else
#include "backends/imgui_impl_sdl2.h"
#define UI_SDL(name) ImGui_ImplSDL2_##name
#endif
static ImGuiContext *s_menu_context, *s_previous_context;
int psx_runtime_menu_gl_begin(void *window, void *context) {
    if (!window || !context || s_menu_context) return 0;
    s_previous_context = ImGui::GetCurrentContext();
    s_menu_context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    /* Use the same bundled typeface and body size as the launcher. */
    auto base = SDL_GetBasePath();
    std::filesystem::path font = base ? base : "";
    font /= "assets/fonts/LatoLatin-Regular.ttf";
#if !defined(PSX_SDL3)
    SDL_free(base);
#endif
    ImFontConfig font_config;
    font_config.SizePixels = launcher_theme_by_name("psx").font_body;
    std::error_code ec;
    if (std::filesystem::is_regular_file(font, ec))
        ImGui::GetIO().Fonts->AddFontFromFileTTF(font.string().c_str(),
                                              font_config.SizePixels, &font_config);
    if (ImGui::GetIO().Fonts->Fonts.empty())
        ImGui::GetIO().Fonts->AddFontDefault(&font_config);
    if (!UI_SDL(InitForOpenGL)((SDL_Window*)window, context)) {
        ImGui::DestroyContext(s_menu_context);
        s_menu_context = nullptr;
        ImGui::SetCurrentContext(s_previous_context);
        return 0;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        UI_SDL(Shutdown)();
        ImGui::DestroyContext(s_menu_context);
        s_menu_context = nullptr;
        ImGui::SetCurrentContext(s_previous_context);
        return 0;
    }
    psx_runtime_menu_set_modern(1);
    return 1;
}
void psx_runtime_menu_gl_event(const void *event) {
    if (s_menu_context) UI_SDL(ProcessEvent)((const SDL_Event*)event);
}
void psx_runtime_menu_gl_draw(void*) {
    if (!s_menu_context) return;
    ImGui_ImplOpenGL3_NewFrame();
    UI_SDL(NewFrame)();
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(psx_runtime_menu_model());
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
void psx_runtime_menu_gl_end(void) {
    if (!s_menu_context) return;
    ImGui_ImplOpenGL3_Shutdown();
    UI_SDL(Shutdown)();
    ImGui::DestroyContext(s_menu_context);
    s_menu_context = nullptr;
    ImGui::SetCurrentContext(s_previous_context);
    psx_runtime_menu_set_modern(0);
}
#else
int psx_runtime_menu_gl_begin(void*, void*) { return 0; }
void psx_runtime_menu_gl_event(const void*) {}
void psx_runtime_menu_gl_draw(void*) {}
void psx_runtime_menu_gl_end(void) {}
#endif
