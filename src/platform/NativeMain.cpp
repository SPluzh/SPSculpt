#define SDL_MAIN_HANDLED
#define _USE_MATH_DEFINES
#include <cmath>
#include <iostream>
#include <csignal>
#include <stdexcept>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <chrono>
#include <limits>

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include "render/AngleRenderer.h"
#include "mesh/Octree.h"
#include "mesh/NormalCalc.h"
#include "sculpt/SculptEngine.h"
#include "mesh/Topology.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "gui/GuiManager.h"
#include "platform/HotkeyDispatcher.h"

#ifdef _WIN32
#include <windows.h>
LONG WINAPI windowsExceptionFilter(struct _EXCEPTION_POINTERS* ExceptionInfo) {
    std::cerr << "\n=============================================" << std::endl;
    std::cerr << "[CRITICAL ERROR] Windows Unhandled Exception! Code: 0x" 
              << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode << std::dec << std::endl;
    std::cerr << "=============================================" << std::endl;
    system("pause");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void crashHandler(int signum) {
    std::cerr << "\n=============================================" << std::endl;
    std::cerr << "[CRITICAL ERROR] Application crashed! Signal: " << signum;
    switch (signum) {
        case SIGSEGV: std::cerr << " (Segmentation Fault / Access Violation)"; break;
        case SIGABRT: std::cerr << " (Abort / Assertion Failure)"; break;
        case SIGFPE:  std::cerr << " (Floating Point Exception)"; break;
        case SIGILL:  std::cerr << " (Illegal Instruction)"; break;
        case SIGINT:  std::cerr << " (Interrupt)"; break;
        case SIGTERM: std::cerr << " (Termination Request)"; break;
    }
    std::cerr << std::endl;
    std::cerr << "=============================================" << std::endl;
    #ifdef _WIN32
    system("pause");
    #else
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();
    #endif
    exit(signum);
}

GLuint generateClayMatcapTexture() {
    const int width = 256;
    const int height = 256;
    std::vector<uint8_t> pixels(width * height * 4);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Normalized coordinates from -1 to 1
            float nx = (x - width * 0.5f) / (width * 0.5f);
            float ny = (height * 0.5f - y) / (height * 0.5f);
            float r2 = nx * nx + ny * ny;
            
            float r, g, b, a;
            if (r2 <= 1.0f) {
                float nz = std::sqrt(1.0f - r2);
                glm::vec3 normal(nx, ny, nz);
                
                // Diffuse lighting from top-right-front light source
                glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, 0.8f, 1.0f));
                float diffuse = std::max(glm::dot(normal, lightDir), 0.0f);
                glm::vec3 lightColor = glm::vec3(0.9f, 0.85f, 0.8f) * diffuse + glm::vec3(0.18f, 0.18f, 0.22f);
                
                // Clay base color
                glm::vec3 clayColor(0.72f, 0.52f, 0.45f);
                glm::vec3 color = clayColor * lightColor;
                
                // Specular highlight
                glm::vec3 rVec = glm::reflect(glm::vec3(0.0f, 0.0f, -1.0f), normal);
                float spec = std::pow(std::max(glm::dot(rVec, lightDir), 0.0f), 16.0f);
                color += glm::vec3(0.15f) * spec;
                
                // Clamp
                color = glm::clamp(color, 0.0f, 1.0f);
                
                r = color.r;
                g = color.g;
                b = color.b;
                a = 1.0f;
            } else {
                // Background color outside sphere (alpha 0 or black)
                r = 0.08f;
                g = 0.09f;
                b = 0.1f;
                a = 0.0f;
            }
            
            int idx = (y * width + x) * 4;
            pixels[idx] = static_cast<uint8_t>(r * 255.0f);
            pixels[idx + 1] = static_cast<uint8_t>(g * 255.0f);
            pixels[idx + 2] = static_cast<uint8_t>(b * 255.0f);
            pixels[idx + 3] = static_cast<uint8_t>(a * 255.0f);
        }
    }

    GLuint texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return texId;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(windowsExceptionFilter);
#endif

    // Register crash signal handlers
    std::signal(SIGSEGV, crashHandler);
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE, crashHandler);
    std::signal(SIGILL, crashHandler);
    std::signal(SIGINT, crashHandler);
    std::signal(SIGTERM, crashHandler);

    try {
        SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    std::cout << "Starting SculptSP Desktop Core..." << std::endl;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return -1;
    }

    // Request OpenGL ES 3.0 Context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    int width = 1280;
    int height = 720;

    SDL_Window* window = SDL_CreateWindow(
        "SculptSP Native Engine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "Failed to create OpenGL ES context: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Initialize the renderer
    AngleRenderer renderer;
    if (!renderer.init(width, height)) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    Scene scene;
    scene.loadDefaultSphere();
    scene.getCamera().onResize(width, height);

    // Apply matcap clay preset to the default sphere
    Mesh* mesh = scene.getSelected();
    if (mesh) {
        mesh->matcapIdx = 5; // "Clay" matcap preset index
        mesh->shaderType = 1; // MATCAP
        mesh->textureId = 0;
    }

    SculptManager sculpt;
    GuiManager gui;
    gui.init(window, glContext);
    HotkeyDispatcher dispatcher;

    bool quit = false;
    SDL_Event event;
    std::cout << "[Debug] Entering main loop." << std::endl;
    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    width = event.window.data1;
                    height = event.window.data2;
                    renderer.resize(width, height);
                    scene.getCamera().onResize(width, height);
                }
            } else {
                ImGui_ImplSDL2_ProcessEvent(&event);

                bool handledByHotkey = dispatcher.processEvent(event, sculpt, scene, gui);

                if (!handledByHotkey) {
                    ImGuiIO& io = ImGui::GetIO();
                    bool skipSculpt = false;
                    if (io.WantCaptureMouse && (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEWHEEL)) {
                        skipSculpt = true;
                    }
                    if (io.WantCaptureKeyboard && (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
                        skipSculpt = true;
                    }

                    if (!skipSculpt) {
                        sculpt.handleEvent(event, scene);
                    }
                }
            }
        }

        sculpt.processFrame(scene);
        sculpt.getCursor().applyToRenderer(renderer);
        renderer.render(scene);
        gui.render(sculpt, scene, renderer, window);

        SDL_GL_SwapWindow(window);
        SDL_Delay(8); // limit to ~120fps
    }

    gui.shutdown();

    std::cout << "Cleaning up..." << std::endl;
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
} catch (const std::exception& e) {
        std::cerr << "\n=============================================" << std::endl;
        std::cerr << "[UNHANDLED EXCEPTION] " << e.what() << std::endl;
        std::cerr << "=============================================" << std::endl;
        #ifdef _WIN32
        system("pause");
        #else
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        #endif
        return -1;
    } catch (...) {
        std::cerr << "\n=============================================" << std::endl;
        std::cerr << "[UNHANDLED EXCEPTION] Unknown exception occurred." << std::endl;
        std::cerr << "=============================================" << std::endl;
        #ifdef _WIN32
        system("pause");
        #else
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        #endif
        return -1;
    }
}
