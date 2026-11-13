cmake_minimum_required(VERSION 3.20)
project(kamenriderbattridewarsport
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
# The port's fixes to the pinned SDK (patches/ps3recomp/, applied by
# docs/BUILD.md and scripts/build_appimage.py). Without them the build would
# work but the chapter 4 stage's audio breaks up, so refuse to build.
file(STRINGS "${PS3RECOMP_DIR}/runtime/spu/spu_helpers.h" _port_sdk_patched LIMIT_COUNT 1 REGEX "spu__x_perm")
if(NOT _port_sdk_patched)
    message(FATAL_ERROR "The SDK at ${PS3RECOMP_DIR} lacks patches/ps3recomp/0001-spu-helpers-sse41-byte-rotates.patch; "
                        "apply it with: git -C ${PS3RECOMP_DIR} apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/ps3recomp/*.patch")
endif()
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# Target CPU
# ---------------------------------------------------------------------------
# The translated PPU and SPU code is mostly scalar loops over vector lanes and
# fused multiply-adds (fmaf). Built for baseline x86-64, every fused
# multiply-add is a libm call and nothing vectorises; in crowded combat the
# game's render thread and the audio SPU thread then ran out of time (about
# 20 fps, a quarter of the audio blocks late). PORT_MARCH=native builds for the
# CPU of the machine running the build -- what a port built from source on
# that machine wants. Set it to a level such as x86-64-v3, or empty for the
# compiler default, to build a binary for other machines.
set(PORT_MARCH "native" CACHE STRING "Value for -march= (empty: compiler default)")
if(PORT_MARCH AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # No implicit contraction: a separate guest multiply and add must stay two
    # roundings, as on the PPU/SPU. Explicit fmaf() becomes one instruction.
    add_compile_options("-march=${PORT_MARCH}" "-ffp-contract=off")
    message(STATUS "Target CPU:      -march=${PORT_MARCH}")
endif()

# The AppImage builds the runner on the player's computer. Keep the static
# shader-tool symbols hidden there: an exported SPIRV-Tools copy can break RADV.
option(PORT_PORTABLE "Build the runner as the AppImage does" OFF)
if(PORT_PORTABLE)
    add_link_options("LINKER:--exclude-libs,ALL")
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# Port-local POSIX device pacing and audio diagnostics, sharing SDK HLE state.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
    get_target_property(PORT_RUNTIME_SOURCES ps3recomp_runtime SOURCES)
    list(FILTER PORT_RUNTIME_SOURCES EXCLUDE REGEX "libs/audio/cellAudio\\.c$")
    set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_RUNTIME_SOURCES}")
    target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/audio_bridge.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/audio_bridge.c" TARGET_DIRECTORY ps3recomp_runtime PROPERTIES COMPILE_DEFINITIONS "PORT_AUDIO_SOURCE=\"${PS3RECOMP_DIR}/libs/audio/cellAudio.c\"")
endif()

# Keyboard and mouse as pad port 0, around the SDK's controller poll.
get_target_property(PORT_PAD_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_PAD_SOURCES EXCLUDE REGEX "libs/input/cellPad\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_PAD_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/input_bridge.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/input_bridge.c" TARGET_DIRECTORY ps3recomp_runtime
    PROPERTIES COMPILE_DEFINITIONS "PORT_CELLPAD_SOURCE=\"${PS3RECOMP_DIR}/libs/input/cellPad.c\"")

# Guest lwmutex locks wait on Win32-style semaphores; skip the clock reads a
# zero-timeout wait on them does not need (sync_bridge.c).
if(NOT WIN32)
    get_target_property(PORT_SYNC_SOURCES ps3recomp_runtime SOURCES)
    list(FILTER PORT_SYNC_SOURCES EXCLUDE REGEX "runtime/platform/win32_compat\\.c$")
    set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_SYNC_SOURCES}")
    target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sync_bridge.c")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/sync_bridge.c" TARGET_DIRECTORY ps3recomp_runtime
        PROPERTIES COMPILE_DEFINITIONS "PORT_WIN32_COMPAT_SOURCE=\"${PS3RECOMP_DIR}/runtime/platform/win32_compat.c\"")
endif()

# The synchronous renderer must not skip FIFO flip boundaries.
get_target_property(PORT_GCM_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_GCM_SOURCES EXCLUDE REGEX "libs/video/cellGcmSys\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_GCM_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gcm_bridge.c")

# Port-local Vulkan vertex texture descriptors and samplers.
get_target_property(PORT_VK_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_VK_SOURCES EXCLUDE REGEX "libs/video/rsx_vulkan_backend\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_VK_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_bridge.c"
                                         "${CMAKE_CURRENT_SOURCE_DIR}/fsr1_bridge.c"
                                         "${CMAKE_CURRENT_SOURCE_DIR}/thread_affinity.c")

# Port-local render diagnostics; capture only when explicitly triggered.
get_target_property(PORT_RENDER_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_RENDER_SOURCES EXCLUDE REGEX "libs/video/rsx_draw_engine\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_RENDER_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rsx_engine_bridge.c")

# The audio task is interpreted, including runtime-loaded firmware code: a
# port-local interpreter with a decode cache (spu_interp_bridge.c).
get_target_property(PORT_SPU_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_SPU_SOURCES EXCLUDE REGEX "runtime/spu/spu_interp\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_SPU_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/spu_interp_bridge.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/spu_interp_bridge.c" TARGET_DIRECTORY ps3recomp_runtime
    PROPERTIES INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")

# Measured FIFO hotspot: four-component vertex decoding, with exact SIMD paths.
get_target_property(PORT_VERTEX_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_VERTEX_SOURCES EXCLUDE REGEX "libs/video/rsx_vertex_compact\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_VERTEX_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rsx_vertex_bridge.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/rsx_vertex_bridge.c" TARGET_DIRECTORY ps3recomp_runtime
    PROPERTIES INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/libs/video")

# The SDK's SPU reservation tracking, with its bitmap readable in place.
get_target_property(PORT_SPU_COH_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_SPU_COH_SOURCES EXCLUDE REGEX "runtime/spu/spu_coherency\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_SPU_COH_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/spu_coherency_bridge.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/spu_coherency_bridge.c" TARGET_DIRECTORY ps3recomp_runtime
    PROPERTIES COMPILE_DEFINITIONS "PORT_SPU_COHERENCY_SOURCE=\"${PS3RECOMP_DIR}/runtime/spu/spu_coherency.c\""
               INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")

# The SDK's channel/MFC layer with cached environment lookups.
get_target_property(PORT_SPU_CH_SOURCES ps3recomp_runtime SOURCES)
list(FILTER PORT_SPU_CH_SOURCES EXCLUDE REGEX "runtime/spu/spu_channels\\.c$")
set_property(TARGET ps3recomp_runtime PROPERTY SOURCES "${PORT_SPU_CH_SOURCES}")
target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/spu_channels_bridge.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/spu_channels_bridge.c" TARGET_DIRECTORY ps3recomp_runtime
    PROPERTIES COMPILE_DEFINITIONS "PORT_SPU_CHANNELS_SOURCE=\"${PS3RECOMP_DIR}/runtime/spu/spu_channels.c\""
               INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")

# Keep the interpreter and DMA/channel helpers optimized, with SSE4.1 so the
# SPU helpers take their vector paths (spu_shufb is a byte loop without it).
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_interp_bridge.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_channels_bridge.c"
        TARGET_DIRECTORY ps3recomp_runtime
        PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1")
endif()

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${CMAKE_CURRENT_SOURCE_DIR}/loader_bridge.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs_bridge.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    # Only the accepted static image; the rejected image uses spu_bridge.c.
    set(SPU_LIFTED "${SPU_DIR}/spu_615f01318fb1c02fbb03cd0fdca61b2144aaed4846bb2fc12fe4ca8fe2dc0657/spu_recomp.c")
    if(EXISTS "${SPU_LIFTED}")
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        # The audio task's code below its data tail, the module it loads and,
        # when the firmware was available, its MP3 codec
        # (scripts/lift_audio_spu.py); spu_bridge.c
        # falls back to pure interpretation without them.
        if(EXISTS "${SPU_DIR}/battride_audio/task/spu_recomp.c")
            list(APPEND SPU_SOURCES "${SPU_DIR}/battride_audio/task/spu_recomp.c")
            message(STATUS "  audio SPU task: translated (battride_audio)")
            if(EXISTS "${SPU_DIR}/battride_audio/mp3/spu_recomp.c")
                list(APPEND SPU_SOURCES "${SPU_DIR}/battride_audio/mp3/spu_recomp.c")
                message(STATUS "  audio SPU MP3 codec: translated from the local firmware")
            endif()
            if(EXISTS "${SPU_DIR}/battride_audio/module/spu_recomp.c")
                list(APPEND SPU_SOURCES "${SPU_DIR}/battride_audio/module/spu_recomp.c")
                message(STATUS "  audio SPU module: translated")
            endif()
        else()
            message(STATUS "  (no battride_audio -- audio SPU task fully interpreted)")
        endif()
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(FATAL_ERROR "SPU sources missing in ${SPU_DIR}. Run scripts/generate.py first, or explicitly use PORT_ENABLE_SPU=OFF for PPU-only diagnostics.")
    endif()
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    sail_bridge.cpp
    spu_bridge.c
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1")
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

# Optional GCC PGO for the translated PPU and C++ harness. Keep the same
# build directory for training and use: GCC embeds object paths in gcda names.
set(PORT_PGO "off" CACHE STRING "GCC PPU profiling: off, generate, use")
set_property(CACHE PORT_PGO PROPERTY STRINGS off generate use)
set(PORT_PGO_DIR "${CMAKE_CURRENT_BINARY_DIR}/pgo" CACHE PATH "Private GCC profile directory")
if(NOT PORT_PGO MATCHES "^(off|generate|use)$")
    message(FATAL_ERROR "PORT_PGO must be off, generate, or use")
endif()
if(NOT PORT_PGO STREQUAL "off")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "PORT_PGO currently requires GCC for C++")
    endif()
    # Profile files are named after each object's path relative to the build
    # directory, so profiles trained in one tree apply to a build in another
    # (the AppImage builds in a fresh directory on the player's computer).
    # MinGW GCC compares the prefix with the working directory as Windows
    # spells it (C:\...), so give it that form there; with '/' the whole
    # absolute path stays in the names.
    set(PORT_PGO_PREFIX "${CMAKE_CURRENT_BINARY_DIR}")
    if(WIN32)
        file(TO_NATIVE_PATH "${PORT_PGO_PREFIX}" PORT_PGO_PREFIX)
    endif()
    target_compile_options(${PROJECT_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fprofile-prefix-path=${PORT_PGO_PREFIX}>")
    if(PORT_PGO STREQUAL "generate")
        target_compile_options(${PROJECT_NAME} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fprofile-generate=${PORT_PGO_DIR}>"
            "$<$<COMPILE_LANGUAGE:CXX>:-fprofile-update=atomic>")
        target_link_options(${PROJECT_NAME} PRIVATE "-fprofile-generate=${PORT_PGO_DIR}")
        target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_PGO_GENERATE=1)
    else()
        file(GLOB_RECURSE PORT_PGO_DATA "${PORT_PGO_DIR}/*.gcda")
        if(NOT PORT_PGO_DATA)
            message(FATAL_ERROR "No GCC training profiles in PORT_PGO_DIR")
        endif()
        target_compile_options(${PROJECT_NAME} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fprofile-use=${PORT_PGO_DIR}>"
            "$<$<COMPILE_LANGUAGE:CXX>:-fprofile-correction>"
            "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=coverage-mismatch>")
    endif()
endif()

# Port-local overrides of SDK headers the lifted code includes (ppu_vmx.h:
# SIMD float ops). Searched before the SDK's own copies.
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC, -mbig-obj on MinGW. A single lifted file routinely passes
#     the 65,536-section limit of the default object format.
set(PORT_LIFTED_OPTIONS "-w;-ffp-contract=off")
if(WIN32 AND NOT MSVC)
    list(APPEND PORT_LIFTED_OPTIONS "-Wa,-mbig-obj")
endif()
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${PORT_LIFTED_OPTIONS}")
endif()

# Hot animation/transform routines observed in the first mission.
# Preserve guest arithmetic flags, optimize this generated unit only.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties("${RECOMP_DIR}/ppu_recomp_017.cpp" PROPERTIES
        COMPILE_OPTIONS "${PORT_LIFTED_OPTIONS};-O2")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

set_source_files_properties(spu_bridge.c PROPERTIES INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu;${SPU_DIR}")

set_source_files_properties(fs_bridge.cpp PROPERTIES COMPILE_DEFINITIONS "PORT_PPU_FS_SOURCE=\"${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp\"")

# The combat CPU profile identifies func_004B5120 (tree traversal) in chunk
# 029 as a hot PPU path. Optimize that translation unit more aggressively,
# keeping the rest at the preset's O2 and preserving strict FP contraction.
option(PORT_HOT_PPU_O3 "Use O3 for the profiled hot PPU chunk" ON)
if(PORT_HOT_PPU_O3 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(source IN LISTS RECOMP_SOURCES)
        if(source MATCHES "ppu_recomp_029\\.cpp$")
            set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -O3)
        endif()
    endforeach()
endif()

# Memory helpers dominate the busy PPU thread's combat profile. Apply the
# guarded inline paths only to profiled chunks, avoiding growth in cold code.
option(PORT_PPU_FAST_MEMORY "Inline guarded PPU memory accesses in hot chunks" ON)
set(PORT_PPU_FAST_MEMORY_CHUNKS "015;017;021;029;030" CACHE STRING "Generated PPU chunks using guarded inline memory")
if(PORT_PPU_FAST_MEMORY AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND (NOT WIN32 OR MINGW))
    foreach(chunk IN LISTS PORT_PPU_FAST_MEMORY_CHUNKS)
        set(source "${RECOMP_DIR}/ppu_recomp_${chunk}.cpp")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing PPU memory optimization chunk: ${chunk}")
        endif()
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
            "-include" "${CMAKE_CURRENT_SOURCE_DIR}/include/port_ppu_memory.h")
    endforeach()
endif()
# Native replacements for hot translated functions (ppu_native.cpp). The
# chunk that defines each is force-included with port_ppu_native.h, which
# makes the lifted definition weak; a lift that put the function elsewhere
# leaves the replacement off rather than failing to link.
option(PORT_PPU_NATIVE "Native replacements for hot translated PPU functions" ON)
set(PORT_PPU_NATIVE_CHUNK "029" CACHE STRING "Generated PPU chunk defining func_004B5120")
if(PORT_PPU_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND (NOT WIN32 OR MINGW))
    set(source "${RECOMP_DIR}/ppu_recomp_${PORT_PPU_NATIVE_CHUNK}.cpp")
    set(native_defined "")
    if(EXISTS "${source}")
        file(STRINGS "${source}" native_defined REGEX "^void func_004B5120\\(ppu_context\\* ctx\\) {")
    endif()
    if(native_defined)
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS
            "-include" "${CMAKE_CURRENT_SOURCE_DIR}/include/port_ppu_native.h")
        target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ppu_native.cpp")
        message(STATUS "Native PPU:      func_004B5120 (chunk ${PORT_PPU_NATIVE_CHUNK})")
    else()
        message(STATUS "Native PPU:      off (func_004B5120 not in chunk ${PORT_PPU_NATIVE_CHUNK})")
    endif()
endif()
# The same fast path for every other translated store, out of line: the
# linker sends the chunks' vm_write8/16/32/64 calls to store_wrap.c.
option(PORT_PPU_WRAP_STORES "Route translated stores through the fast path in store_wrap.c" ON)
if(PORT_PPU_WRAP_STORES AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND (NOT WIN32 OR MINGW) AND NOT APPLE)
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/store_wrap.c")
    target_link_options(${PROJECT_NAME} PRIVATE
        "-Wl,--wrap=vm_write8,--wrap=vm_write16,--wrap=vm_write32,--wrap=vm_write64")
endif()
