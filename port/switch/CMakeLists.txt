# WWE SmackDown vs. Raw 2011 on Nintendo Switch (Horizon, libnx, GCC).
# See port/docs/SWITCH_PORT_PLAN.md. Configure with devkitPro's Switch.cmake from
# MSYS2 (tools: port/switch/tools); paths must have no spaces for its tools, so the
# SDK, plume and generated code are private copies in /work/nx.
cmake_minimum_required(VERSION 3.25)
project(svr2011_switch LANGUAGES C CXX ASM)

set(SVR_PORT "/work/nx/port" CACHE PATH "port folder (a link without spaces: the tools need that)")
set(REXSDK_DIR "/work/nx/rex" CACHE PATH "the SDK, with the svr2011 and Switch patches")
set(PLUME_DIR "/work/nx/plume" CACHE PATH "plume, with the svr2011 patch")
set(SVR_GENERATED "/work/nx/generated" CACHE PATH "generated recompiled code")
set(NVK_SDK "/work/nvk-sdk" CACHE PATH "mesa-switch: libvulkan.a and its parts")
option(SVR_SDK_ONLY "Only the SDK's libraries" OFF)
option(SVR_NX_REPORTS_LIVE "Release builds: Report a problem may use the public server (test builds never do)" OFF)
# Link-time and profile-guided optimisation (port/docs/SWITCH_BUILD.md, "LTO and PGO").
option(SVR_NX_LTO "The game's code and the port's own code optimised as one program at the link" OFF)
set(SVR_NX_LTO_JOBS "4" CACHE STRING "Parallel jobs of the LTO link")
set(SVR_NX_PGO "" CACHE STRING "Profile-guided optimisation: empty, generate (a build that counts) or use")
set(SVR_NX_PGO_DIR "/work/nx/pgo" CACHE PATH "Folder of the profile (.gcda files)")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# For everything compiled for the console, the SDK too:
# -mearly-ra=none: GCC 16's AArch64 early register allocation (on at -O2) has miscompiled NEON table lookups that need
#   two consecutive registers (the recompiled code's vector permutes) in other Switch ports of recompiled games.
# -mno-outline-atomics: GCC's default calls a helper per atomic that tests for LSE at run time; the A57 has none, so
#   the same load-exclusive / store-exclusive loop is written in place instead.
add_compile_options(-mearly-ra=none -mno-outline-atomics)
add_link_options(-mearly-ra=none)

set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_HOST_TOOL "/bin/true" CACHE FILEPATH "" FORCE)
add_subdirectory(${REXSDK_DIR} rexglue-sdk)

if(SVR_SDK_ONLY)
  return()
endif()

# What the SDK's own folders are built with (its settings don't reach this one).
add_compile_definitions(REX_PLATFORM_SWITCH=1 REX_PLATFORM_LINUX=1 NDEBUG REXGLUE_BUILD_CONFIG="Release")
# (-fPIE after CMake's -fPIC: nothing here is interposed, so calls can inline and own data needs no table.)
add_compile_options(-fno-strict-aliasing -w -fPIE -fno-semantic-interposition
  "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${REXSDK_DIR}/compat_nx/rex_nx_compat.h>")
include_directories(BEFORE SYSTEM ${REXSDK_DIR}/compat_nx)
get_filename_component(SVR_GENERATED_PARENT "${SVR_GENERATED}" DIRECTORY)

# ---- recompiled game code ----
set(CMAKE_CURRENT_SOURCE_DIR_SAVED "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB SVR_RECOMP CONFIGURE_DEPENDS "${SVR_GENERATED}/default/*.cpp")
add_library(svr2011_recomp OBJECT ${SVR_RECOMP})
target_include_directories(svr2011_recomp PRIVATE ${SVR_GENERATED} ${SVR_GENERATED}/default ${SVR_PORT}/src)
target_link_libraries(svr2011_recomp PRIVATE rex::runtime)
target_precompile_headers(svr2011_recomp PRIVATE "${SVR_GENERATED}/default/svr2011_pch.h")
target_compile_options(svr2011_recomp PRIVATE -O2 -g0 -fno-math-errno -fno-trapping-math -fomit-frame-pointer)

# ---- the game's own code: the same list as port/CMakeLists.txt ----
file(READ "${SVR_PORT}/CMakeLists.txt" SVR_TOP)
string(REGEX MATCH "set\\(SVR2011_SOURCES[^)]*\\)" SVR_LIST "${SVR_TOP}")
string(REGEX MATCHALL "(src|modmaker/svrfmt)/[A-Za-z0-9_/]+\\.cpp" SVR_FILES "${SVR_LIST}")
list(APPEND SVR_FILES
  src/model_mem.cpp src/mem_trace.cpp
  src/native/gpu.cpp src/native/backend_vulkan.cpp src/native/recording_list.cpp
  src/native/native_renderer.cpp src/native/native_hooks.cpp src/native/textures.cpp
  src/native/texture_packs.cpp
  src/posix_stubs.cpp)
set(SVR_SOURCES "")
foreach(f ${SVR_FILES})
  list(APPEND SVR_SOURCES "${SVR_PORT}/${f}")
endforeach()
list(APPEND SVR_SOURCES
  ${REXSDK_DIR}/src/graphics/pipeline/texture/util.cpp
  ${REXSDK_DIR}/src/graphics/pipeline/texture/info_formats.cpp
  ${REXSDK_DIR}/src/ui/windowed_app_main_sdl.cpp
  ${REXSDK_DIR}/src/ui/rex_app.cpp
  ${SVR_PORT}/launcher/bink_decode.c
  ${SVR_PORT}/launcher/bink_encode.c
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_stubs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_profile.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_ab.cpp ${CMAKE_CURRENT_SOURCE_DIR}/nx_fast.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_report.cpp ${CMAKE_CURRENT_SOURCE_DIR}/nx_install_check.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sdl3_nx/sdl_nx.cpp
  ${PLUME_DIR}/plume_vulkan.cpp)

file(STRINGS "${SVR_PORT}/VERSION.txt" SVR_VERSION LIMIT_COUNT 1)
add_executable(svr2011 ${SVR_SOURCES} $<TARGET_OBJECTS:svr2011_recomp>)
target_include_directories(svr2011 PRIVATE
  ${SVR_PORT} ${SVR_PORT}/src ${SVR_PORT}/launcher ${SVR_PORT}/modmaker/svrfmt ${SVR_GENERATED_PARENT} ${SVR_GENERATED} ${SVR_GENERATED}/default ${PLUME_DIR}
  ${PLUME_DIR}/contrib ${PLUME_DIR}/contrib/volk ${PLUME_DIR}/contrib/Vulkan-Headers/include
  ${PLUME_DIR}/contrib/VulkanMemoryAllocator/include
  $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
  ${REXSDK_DIR}/thirdparty/xxHash ${REXSDK_DIR}/thirdparty/stb)
target_compile_options(svr2011 PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${SVR_PORT}/src/compat.h>")
target_compile_definitions(svr2011 PRIVATE
  SVR2011_PORT_VERSION="${SVR_VERSION}" SVR2011_BUILD_COMMIT="switch"
  PLUME_SWITCH=1)
if(SVR_NX_REPORTS_LIVE)
  target_compile_definitions(svr2011 PRIVATE SVR_NX_REPORTS_LIVE=1)
endif()
# (nx_report.cpp: zlib and Mbed TLS, from devkitPro's portlibs)
find_path(NX_PORTLIBS_INCLUDE mbedtls/ssl.h REQUIRED)
target_include_directories(svr2011 PRIVATE ${NX_PORTLIBS_INCLUDE})
target_compile_options(svr2011 PRIVATE -O2)

# ---- LTO and PGO ----
# LTO: both targets are compiled to GCC's intermediate form only and optimised together at the link. The SDK's
# libraries and the driver stay as they are.
# PGO generate: every branch of both targets is counted; nx_pgo.cpp writes the counts on the console (libgcov's own
# way, a file per object at exit at the build machine's paths, doesn't fit: the game is closed from HOME, and those
# paths aren't there). The counts are registered in a section instead of by constructors.
# PGO use: the counts, merged on the PC (tools/pgo_merge.sh), steer inlining, layout and branches. Objects are named
# relative to the build folder, so the two builds may be in different folders. Code the sessions never ran is compiled
# as without a profile.
if(SVR_NX_LTO)
  foreach(t svr2011_recomp svr2011)
    target_compile_options(${t} PRIVATE -flto -fno-fat-lto-objects)
  endforeach()
  # (these three define the --wrap replacements, which the linker pairs up by name)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/nx_main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/nx_stubs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/nx_profile.cpp PROPERTIES COMPILE_OPTIONS -fno-lto)
  target_link_options(svr2011 PRIVATE -flto=${SVR_NX_LTO_JOBS} -flto-partition=balanced -O2 -fno-strict-aliasing
    -fno-math-errno -fno-trapping-math -fPIE -fno-semantic-interposition)
  message(STATUS "svr2011: LTO (${SVR_NX_LTO_JOBS} jobs)")
endif()
if(SVR_NX_PGO STREQUAL "generate")
  set(SVR_PGO_FLAGS "-fprofile-generate=${SVR_NX_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
    -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
  target_sources(svr2011 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nx_pgo.cpp)
  # (its own counts aren't wanted, and it must not count while it reads the others)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/nx_pgo.cpp PROPERTIES COMPILE_OPTIONS
    "-fno-profile-generate;-fno-lto")
  target_compile_definitions(svr2011 PRIVATE SVR_NX_PGO_GENERATE=1)
elseif(SVR_NX_PGO STREQUAL "use")
  file(GLOB SVR_PGO_GCDA "${SVR_NX_PGO_DIR}/*.gcda")
  if(NOT SVR_PGO_GCDA)
    message(FATAL_ERROR "SVR_NX_PGO=use: no .gcda files in ${SVR_NX_PGO_DIR}")
  endif()
  # (-fprofile-correction: the counts were taken without atomic updates on several threads, so a few don't add up)
  set(SVR_PGO_FLAGS "-fprofile-use=${SVR_NX_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
    -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch)
elseif(SVR_NX_PGO)
  message(FATAL_ERROR "SVR_NX_PGO must be empty, generate or use (got ${SVR_NX_PGO})")
endif()
if(SVR_NX_PGO)
  foreach(t svr2011_recomp svr2011)
    target_compile_options(${t} PRIVATE ${SVR_PGO_FLAGS})
  endforeach()
  target_link_options(svr2011 PRIVATE ${SVR_PGO_FLAGS})
  message(STATUS "svr2011: PGO ${SVR_NX_PGO} (${SVR_NX_PGO_DIR})")
endif()

target_link_directories(svr2011 PRIVATE ${NVK_SDK}/lib)
target_link_libraries(svr2011 PRIVATE rex::runtime rexgpu-xenos
  -Wl,--allow-multiple-definition -Wl,--wrap=svcBreak -Wl,--wrap=stat -Wl,--wrap=lstat -Wl,--wrap=_open_r -Wl,--wrap=_read_r -Wl,--wrap=_lseek_r -Wl,--wrap=_close_r -Wl,--wrap=_write_r -Wl,--wrap=rename -Wl,--wrap=fchmod -Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=closedir -Wl,--wrap=pthread_detach -Wl,--wrap=threadStart -Wl,--start-group
  vulkan nvk nak nak_rs nil libnil_format_table nouveau_horizon nouveau_mme nouveau_ws nvidia_headers_c
  vtn nir compiler compiler_c_helpers vulkan_util mesa_util mesa_util_c11 mesa_util_simd xmlconfig blake3
  expat zstd mbedtls mbedx509 mbedcrypto z nx stdc++ m -Wl,--end-group)
