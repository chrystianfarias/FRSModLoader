# FRSModLoader - a modding platform for NFS Underground 2 (SPEED2.EXE v1.2 NTSC)
#
# Two targets, both 32-bit because they live inside the game:
#   FRSModLoader.asi        - injected into the game: QuickJS + SDK + CEF overlay
#   FRSModLoaderHelper.exe  - CEF's subprocess (renderer, gpu, utility)
#
# CEF comes from third_party/cef (fetch it with tools/fetch_cef.ps1).
# QuickJS comes in through FetchContent on the first configure.

cmake_minimum_required(VERSION 3.21)
project(FRSModLoader CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CEF requires the static runtime (/MT), and the .asi wants it too: it is
# loaded into a 2004 process and cannot depend on a redistributable being
# installed. This has to apply to everything, QuickJS included, or the link
# reports a conflict.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "FRSModLoader is 32-bit (SPEED2.EXE is x86). Configure with -A Win32 "
        "or use the presets: cmake --preset x86")
endif()

# ---------------------------------------------------------------- CEF
set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
if(NOT EXISTS "${CEF_ROOT}/include/cef_version.h")
    message(FATAL_ERROR
        "CEF is missing. Run: powershell -ExecutionPolicy Bypass -File tools/fetch_cef.ps1")
endif()

set(PROJECT_ARCH "x86")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
# Logical target that resolves libcef.lib per configuration (Debug/Release).
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

# ---------------------------------------------------------------- QuickJS
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_QJS_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(quickjs
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG        v0.16.2
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL TRUE)   # the library only; qjs.exe and the tests are noise
FetchContent_MakeAvailable(quickjs)

# ---------------------------------------------------------------- .asi
set(FRSMODLOADER_SOURCES
    src/dllmain.cpp
    src/js/JsRuntime.cpp
    src/js/ModHost.cpp
    src/js/ApiCore.cpp
    src/js/ApiMemory.cpp
    src/js/ApiGame.cpp
    src/js/ApiUi.cpp
    src/js/ApiAudio.cpp
    src/js/ApiEngine.cpp
    src/js/ApiCommands.cpp
    src/js/ApiStore.cpp
    src/js/ApiDraw.cpp
    src/js/ApiSettings.cpp
    src/core/Audio.cpp
    src/ui/D3D9Hook.cpp
    src/ui/Overlay.cpp
    src/ui/Draw2D.cpp
    src/ui/Splash.cpp
    src/ui/Cursor.cpp
    src/ui/ModsMenu.cpp
    src/ui/WorldDraw.cpp
    src/ui/CefHost.cpp
    src/ui/GameData.cpp
    third_party/lzma/LzmaDec.c
    src/ui/GameTextures.cpp
    src/ui/InputRouter.cpp
    src/ui/DInputBlock.cpp
    src/bridge/Bridge.cpp
    src/host/HostApi.cpp
    src/host/ModSettings.cpp
)

add_library(FRSModLoader SHARED ${FRSMODLOADER_SOURCES})
set_target_properties(FRSModLoader PROPERTIES
    OUTPUT_NAME "FRSModLoader"
    SUFFIX ".asi"
    PREFIX "")
target_include_directories(FRSModLoader PRIVATE src third_party/lzma "${CEF_ROOT}")
target_link_libraries(FRSModLoader PRIVATE
    libcef_lib libcef_dll_wrapper qjs
    d3d9 user32 gdi32 ole32 winmm)
target_compile_definitions(FRSModLoader PRIVATE
    WIN32 _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# libcef.dll does not sit next to SPEED2.EXE; it lives in the FRSModLoader
# folder. That is why it is delay-loaded: the .asi loads the DLL by full path
# before the first CEF call (see CefHost.cpp).
target_link_options(FRSModLoader PRIVATE /DELAYLOAD:libcef.dll)
target_link_libraries(FRSModLoader PRIVATE delayimp)

SET_LIBRARY_TARGET_PROPERTIES(FRSModLoader)

# ---------------------------------------------------------------- helper
add_executable(FRSModLoaderHelper WIN32 helper/helper_main.cpp)
target_include_directories(FRSModLoaderHelper PRIVATE "${CEF_ROOT}")
target_link_libraries(FRSModLoaderHelper PRIVATE libcef_lib libcef_dll_wrapper)
SET_EXECUTABLE_TARGET_PROPERTIES(FRSModLoaderHelper)

# ---------------------------------------------------------------- SpawnCar
# The spawn-car mod as a native .asi: it hooks the game on its own and borrows
# FRSModLoader's interface through the same public header as the example.
# plugins/ is not in the repository - it is where the local .asi mods live - so
# the target only exists on a machine that has one.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/spawn-car/SpawnCar.cpp")
    add_library(SpawnCar SHARED plugins/spawn-car/SpawnCar.cpp)
    set_target_properties(SpawnCar PROPERTIES
        OUTPUT_NAME "SpawnCar"
        SUFFIX ".asi"
        PREFIX "")
    target_include_directories(SpawnCar PRIVATE sdk plugins/spawn-car)
    target_compile_definitions(SpawnCar PRIVATE
        WIN32 _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # psapi: the plugin needs its own module's bounds to tell, on a hardware
    # breakpoint, a write of its own from a write by the game.
    target_link_libraries(SpawnCar PRIVATE psapi)
endif()

# ---------------------------------------------------------------- example plugin
# A native .asi that takes its interface from FRSModLoader through sdk/frsmodloader.h
# and nothing else - no CEF, no QuickJS, one source file. It is also what keeps
# the public header honest: it is compiled on every build.
add_library(HelloPanel SHARED examples/asi-plugin/HelloPanel.cpp)
set_target_properties(HelloPanel PROPERTIES
    OUTPUT_NAME "HelloPanel"
    SUFFIX ".asi"
    PREFIX "")
target_include_directories(HelloPanel PRIVATE sdk)
target_compile_definitions(HelloPanel PRIVATE
    WIN32 _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
