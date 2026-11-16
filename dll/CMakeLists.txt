cmake_minimum_required(VERSION 3.15)
project(bo2mvm CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# t6mp.exe is a 32 bit process, so this DLL MUST be built as x86 / Win32.
#   MSVC : cmake -S . -B build -A Win32            then   cmake --build build --config Release
#   MinGW: cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build a 32 bit (Win32) DLL - the game process is 32 bit.")
endif()

set(IMGUI third_party/imgui)
add_library(bo2mvm SHARED
  src/dllmain.cpp src/core.cpp src/overlay.cpp src/capture.cpp src/zombies.cpp src/livefx.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/imgui_impl_win32.cpp ${IMGUI}/imgui_impl_dx11.cpp)
target_include_directories(bo2mvm PRIVATE src ${IMGUI})
target_compile_definitions(bo2mvm PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(bo2mvm PRIVATE d3d11 dxgi user32 gdi32 dwmapi imm32)
if(MSVC)
  target_compile_options(bo2mvm PRIVATE /W3 /MT$<$<CONFIG:Debug>:d>)   # static CRT: nothing extra to install
else()
  target_link_options(bo2mvm PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(bo2mvm PROPERTIES PREFIX "")
