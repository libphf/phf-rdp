cmake_minimum_required(VERSION 3.16)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "ScreenCaptureKit audio needs macOS 13")
  project(phf_rdp_server C OBJC)
else()
  project(phf_rdp_server C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PHF_RDP_OPENMP "Use OpenMP if available" ON)

set(SOURCES src/main.c src/phf.c src/huff.c src/mongoose.c)
option(PHF_RDP_STUB "Use the synthetic test-pattern backend instead of the native one" OFF)

if(WIN32)
  list(APPEND SOURCES src/capture_win_dxgi.c src/tray_win.c)
elseif(APPLE AND NOT PHF_RDP_STUB)
  # ScreenCaptureKit + CGEvent + NSPasteboard
  list(APPEND SOURCES src/capture_mac.m)
  set_source_files_properties(src/capture_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
  # No native capture backend yet: synthetic test pattern, no input/clipboard/audio.
  list(APPEND SOURCES src/capture_stub.c)
endif()

add_executable(phf-rdp-server ${SOURCES})
target_include_directories(phf-rdp-server PRIVATE include src)
set_target_properties(phf-rdp-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
  target_link_libraries(phf-rdp-server PRIVATE d3d11 dxgi dxguid gdi32 user32 ws2_32 winmm ole32 shell32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(phf-rdp-server PRIVATE Threads::Threads m)
  if(APPLE AND NOT PHF_RDP_STUB)
    target_link_libraries(phf-rdp-server PRIVATE
      "-framework ScreenCaptureKit" "-framework CoreMedia" "-framework CoreVideo"
      "-framework CoreGraphics" "-framework ApplicationServices" "-framework Carbon"
      "-framework AppKit" "-framework Foundation")
  endif()
endif()

if(PHF_RDP_OPENMP)
  find_package(OpenMP)
  if(OpenMP_C_FOUND)
    target_link_libraries(phf-rdp-server PRIVATE OpenMP::OpenMP_C)
  endif()
endif()
