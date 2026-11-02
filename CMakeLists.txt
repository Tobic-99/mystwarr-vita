cmake_minimum_required(VERSION 3.16)
project(mystwarr CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(MW_TRACE "function-entry trace of the 68000 (debug)" OFF)
if(MW_TRACE)
  add_compile_definitions(MW_TRACE=1)
endif()
set(GEN_WARN -Wno-parentheses-equality -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-value)

# recompiled code (tools/m68krec.py)
file(GLOB M68K_GEN gen/m68k/*.cpp)
add_library(mw_gen STATIC ${M68K_GEN})
target_include_directories(mw_gen PUBLIC runtime gen/m68k host)
target_compile_options(mw_gen PRIVATE ${GEN_WARN})

# the board: memory map, I/O, scheduler
add_library(mw_board STATIC host/mw_host.cpp host/eeprom.cpp host/scheduler.cpp host/fiber.cpp host/trace.cpp host/png.cpp host/z80.cpp host/sound.cpp host/video.cpp host/roms.cpp)
target_include_directories(mw_board PUBLIC runtime gen/m68k host)

add_executable(mw_desktop host/mw_main.cpp)
target_link_libraries(mw_desktop mw_board mw_gen mw_board)
