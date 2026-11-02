cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()
project(mystwarr_vita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Mystic Warriors")
set(VITA_TITLEID  "MYSW00001")
set(VITA_VERSION  "01.00")
set(CMAKE_CXX_STANDARD 17)
set(R ${CMAKE_SOURCE_DIR}/..)
get_filename_component(ROOT_ABS ${R} ABSOLUTE)
# -ffile-prefix-map: no build folder in the binary, so a build is the same byte for byte from any folder
set(COMMON_FLAGS "-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-strict-aliasing -ffile-prefix-map=${ROOT_ABS}=.")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS}")
set(GEN_WARN -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-value)

file(GLOB M68K_GEN ${R}/gen/m68k/*.cpp)
add_library(mw_gen STATIC ${M68K_GEN})
target_include_directories(mw_gen PUBLIC ${R}/runtime ${R}/gen/m68k ${R}/host)
target_compile_options(mw_gen PRIVATE ${GEN_WARN})
target_compile_definitions(mw_gen PRIVATE MW_VITA=1)

add_executable(mystwarr ${R}/host/mw_host.cpp ${R}/host/eeprom.cpp ${R}/host/scheduler.cpp ${R}/host/fiber.cpp
               ${R}/host/trace.cpp ${R}/host/z80.cpp ${R}/host/sound.cpp ${R}/host/video.cpp ${R}/host/roms.cpp
               ${R}/host/png.cpp ${CMAKE_SOURCE_DIR}/vita_main.cpp)
target_include_directories(mystwarr PRIVATE ${R}/runtime ${R}/gen/m68k ${R}/host)
target_compile_definitions(mystwarr PRIVATE MW_VITA=1)
target_compile_options(mystwarr PRIVATE -include ${CMAKE_SOURCE_DIR}/vita_log.h)
target_link_libraries(mystwarr mw_gen vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceSysmodule_stub SceKernelDmacMgr_stub SceAppMgr_stub
    SceAppUtil_stub SceCommonDialog_stub SceIofilemgr_stub ScePower_stub SceLibKernel_stub SceKernelThreadMgr_stub
    SceRtc_stub SceAudio_stub SceTouch_stub SceMotion_stub m stdc++)
vita_create_self(eboot.bin mystwarr UNSAFE)
# LiveArea images are not in the repository: packed when present (8-bit indexed PNGs)
set(VPK_FILES FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
foreach(ART icon0.png livearea/contents/bg.png livearea/contents/startup.png)
  if(EXISTS ${CMAKE_SOURCE_DIR}/sce_sys/${ART})
    list(APPEND VPK_FILES FILE ${CMAKE_SOURCE_DIR}/sce_sys/${ART} sce_sys/${ART})
  endif()
endforeach()
vita_create_vpk(mystwarr.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME} ${VPK_FILES})
