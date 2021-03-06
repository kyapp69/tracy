#include <assert.h>
#include <inttypes.h>
#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <GL/gl3w.h>
#include <GLFW/glfw3.h>
#include <memory>
#include "../nfd/nfd.h"
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#endif

#include "../../server/TracyBadVersion.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyImGui.hpp"
#include "../../server/TracyStorage.hpp"
#include "../../server/TracyView.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../server/TracyVersion.hpp"
#include "../../server/IconsFontAwesome5.h"

#include "imgui_freetype.h"
#include "Arimo.hpp"
#include "Cousine.hpp"
#include "FontAwesomeSolid.hpp"

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "Error %d: %s\n", error, description);
}

static void OpenWebpage( const char* url )
{
#ifdef _WIN32
    ShellExecuteA( nullptr, nullptr, url, nullptr, nullptr, 0 );
#else
    char buf[1024];
    sprintf( buf, "xdg-open %s", url );
    system( buf );
#endif
}

static GLFWwindow* s_glfwWindow = nullptr;
static bool s_customTitle = false;
static void SetWindowTitleCallback( const char* title )
{
    assert( s_glfwWindow );
    glfwSetWindowTitle( s_glfwWindow, title );
    s_customTitle = true;
}

int main( int argc, char** argv )
{
    std::unique_ptr<tracy::View> view;
    int badVer = 0;

    if( argc == 2 )
    {
        auto f = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( argv[1] ) );
        if( f )
        {
            view = std::make_unique<tracy::View>( *f );
        }
    }

    char title[128];
    sprintf( title, "Tracy server %i.%i.%i", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch );

    std::string winPosFile = tracy::GetSavePath( "window.position" );
    int x = 200, y = 200, w = 1650, h = 960, maximize = 0;
    {
        FILE* f = fopen( winPosFile.c_str(), "rb" );
        if( f )
        {
            uint32_t data[5];
            fread( data, 1, sizeof( data ), f );
            fclose( f );
            x = data[0];
            y = data[1];
            w = data[2];
            h = data[3];
            maximize = data[4];
        }
    }

    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if( !glfwInit() ) return 1;
    glfwWindowHint(GLFW_VISIBLE, 0);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow( w, h, title, NULL, NULL);
    if( !window ) return 1;
    glfwSetWindowPos( window, x, y );
#ifdef GLFW_MAXIMIZED
    if( maximize ) glfwMaximizeWindow( window );
#endif
    s_glfwWindow = window;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync
    gl3wInit();

    float dpiScale = 1.f;
#ifdef _WIN32
    typedef UINT(*GDFS)(void);
    GDFS getDpiForSystem = nullptr;
    HMODULE dll = GetModuleHandleW(L"user32.dll");
    if (dll != INVALID_HANDLE_VALUE)
        getDpiForSystem = (GDFS)GetProcAddress(dll, "GetDpiForSystem");
    if (getDpiForSystem)
        dpiScale = getDpiForSystem() / 96.f;
#endif

    // Setup ImGui binding
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    std::string iniFileName = tracy::GetSavePath( "imgui.ini" );
    io.IniFilename = iniFileName.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOpenGL( window, true );
    ImGui_ImplOpenGL3_Init( "#version 150" );

    static const ImWchar rangesBasic[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x03BC, 0x03BC, // micro
        0,
    };
    static const ImWchar rangesIcons[] = {
        ICON_MIN_FA, ICON_MAX_FA,
        0
    };
    ImFontConfig configMerge;
    configMerge.MergeMode = true;

    io.Fonts->AddFontFromMemoryCompressedTTF( tracy::Arimo_compressed_data, tracy::Arimo_compressed_size, 15.0f * dpiScale, nullptr, rangesBasic );
    io.Fonts->AddFontFromMemoryCompressedTTF( tracy::FontAwesomeSolid_compressed_data, tracy::FontAwesomeSolid_compressed_size, 14.0f * dpiScale, &configMerge, rangesIcons );
    auto fixedWidth = io.Fonts->AddFontFromMemoryCompressedTTF( tracy::Cousine_compressed_data, tracy::Cousine_compressed_size, 15.0f * dpiScale );

    ImGuiFreeType::BuildFontAtlas( io.Fonts, ImGuiFreeType::LightHinting );

    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowBorderSize = 1.f * dpiScale;
    style.FrameBorderSize = 1.f * dpiScale;
    style.FrameRounding = 5.f * dpiScale;
    style.ScrollbarSize *= dpiScale;
    style.Colors[ImGuiCol_WindowBg] = ImVec4( 0.11f, 0.11f, 0.08f, 0.94f );
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4( 1, 1, 1, 0.03f );

    ImVec4 clear_color = ImColor(114, 144, 154);

    char addr[1024] = { "127.0.0.1" };

    std::thread loadThread;

    glfwShowWindow( window );

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        if( glfwGetWindowAttrib( window, GLFW_ICONIFIED ) )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            continue;
        }

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if( !view )
        {
            if( s_customTitle )
            {
                s_customTitle = false;
                glfwSetWindowTitle( window, title );
            }

            ImGui::Begin( "Tracy server", nullptr, ImGuiWindowFlags_AlwaysAutoResize );
            char buf[128];
            sprintf( buf, "Tracy %i.%i.%i", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch );
            tracy::TextCentered( buf );
            if( ImGui::Button( ICON_FA_BOOK " User manual" ) )
            {
                OpenWebpage( "https://bitbucket.org/wolfpld/tracy/downloads/tracy.pdf" );
            }
            ImGui::SameLine();
            if( ImGui::Button( ICON_FA_GLOBE_AMERICAS " Homepage" ) )
            {
                OpenWebpage( "https://bitbucket.org/wolfpld/tracy" );
            }
            ImGui::SameLine();
            if( ImGui::Button( ICON_FA_VIDEO " Tutorial" ) )
            {
                OpenWebpage( "https://www.youtube.com/watch?v=fB5B46lbapc" );
            }
            ImGui::Separator();
            ImGui::Text( "Connect to client" );
            ImGui::InputText( "Address", addr, 1024 );
            if( ImGui::Button( ICON_FA_WIFI " Connect" ) && *addr && !loadThread.joinable() )
            {
                view = std::make_unique<tracy::View>( addr, fixedWidth, SetWindowTitleCallback );
            }
            ImGui::Separator();
            if( ImGui::Button( ICON_FA_FOLDER_OPEN " Open saved trace" ) && !loadThread.joinable() )
            {
                nfdchar_t* fn;
                auto res = NFD_OpenDialog( "tracy", nullptr, &fn );
                if( res == NFD_OKAY )
                {
                    try
                    {
                        auto f = std::shared_ptr<tracy::FileRead>( tracy::FileRead::Open( fn ) );
                        if( f )
                        {
                            loadThread = std::thread( [&view, f, &badVer, fixedWidth] {
                                try
                                {
                                    view = std::make_unique<tracy::View>( *f, fixedWidth, SetWindowTitleCallback );
                                }
                                catch( const tracy::UnsupportedVersion& e )
                                {
                                    badVer = e.version;
                                }
                            } );
                        }
                    }
                    catch( const tracy::NotTracyDump& e )
                    {
                        badVer = -1;
                    }
                }
            }

            if( badVer != 0 )
            {
                if( loadThread.joinable() ) { loadThread.join(); }
                tracy::BadVersion( badVer );
            }

            ImGui::End();
        }
        else
        {
            if( loadThread.joinable() ) loadThread.join();
            view->NotifyRootWindowSize( display_w, display_h );
            if( !view->Draw() )
            {
                view.reset();
            }
        }
        auto& progress = tracy::Worker::GetLoadProgress();
        auto totalProgress = progress.total.load( std::memory_order_relaxed );
        if( totalProgress != 0 )
        {
            ImGui::OpenPopup( "Loading trace..." );
        }
        if( ImGui::BeginPopupModal( "Loading trace...", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            tracy::TextCentered( ICON_FA_HOURGLASS_HALF );
            auto currProgress = progress.progress.load( std::memory_order_relaxed );
            if( totalProgress == 0 )
            {
                ImGui::CloseCurrentPopup();
                totalProgress = currProgress;
            }
            switch( currProgress )
            {
            case tracy::LoadProgress::Initialization:
                ImGui::Text( "Initialization..." );
                break;
            case tracy::LoadProgress::Locks:
                ImGui::Text( "Locks..." );
                break;
            case tracy::LoadProgress::Messages:
                ImGui::Text( "Messages..." );
                break;
            case tracy::LoadProgress::Zones:
                ImGui::Text( "CPU zones..." );
                break;
            case tracy::LoadProgress::GpuZones:
                ImGui::Text( "GPU zones..." );
                break;
            case tracy::LoadProgress::Plots:
                ImGui::Text( "Plots..." );
                break;
            case tracy::LoadProgress::Memory:
                ImGui::Text( "Memory..." );
                break;
            case tracy::LoadProgress::CallStacks:
                ImGui::Text( "Call stacks..." );
                break;
            default:
                assert( false );
                break;
            }
            ImGui::ProgressBar( float( currProgress ) / totalProgress, ImVec2( 200 * dpiScale, 0 ) );

            ImGui::Text( "Progress..." );
            auto subTotal = progress.subTotal.load( std::memory_order_relaxed );
            auto subProgress = progress.subProgress.load( std::memory_order_relaxed );
            if( subTotal == 0 )
            {
                ImGui::ProgressBar( 1.f, ImVec2( 200 * dpiScale, 0 ) );
            }
            else
            {
                ImGui::ProgressBar( float( subProgress ) / subTotal, ImVec2( 200 * dpiScale, 0 ) );
            }
            ImGui::EndPopup();
        }

        // Rendering
        ImGui::Render();
        glfwMakeContextCurrent(window);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwMakeContextCurrent(window);
        glfwSwapBuffers(window);

        if( !glfwGetWindowAttrib( window, GLFW_FOCUSED ) )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
    }

    FILE* f = fopen( winPosFile.c_str(), "wb" );
    if( f )
    {
        glfwGetWindowPos( window, &x, &y );
        glfwGetWindowSize( window, &w, &h );
#ifdef GLFW_MAXIMIZED
        uint32_t maximized = glfwGetWindowAttrib( window, GLFW_MAXIMIZED );
#else
        uint32_t maximized = 0;
#endif

        uint32_t data[5] = { uint32_t( x ), uint32_t( y ), uint32_t( w ), uint32_t( h ), maximized };
        fwrite( data, 1, sizeof( data ), f );
        fclose( f );
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
