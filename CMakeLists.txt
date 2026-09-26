cmake_minimum_required(VERSION 3.20)
# Keep in step with the changelog in README.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.11.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
    "runtime/diag/*.c"      # yz_frontier_trace, used by the live draw engine
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# The Metal backend is Objective-C and Apple-only, so it is added explicitly
# rather than globbed. It needs no platform #ifdef of its own as a result.
if(APPLE)
    enable_language(OBJC)
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m")
endif()
# The HLSL -> MSL translator is C++ (it drives glslang's C++ API) and compiles
# to a stub wherever the shader tools are absent, so it is part of the
# library everywhere. See the "Guest shaders on Metal" section below.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_msl.cpp")
# Same story for the HLSL -> SPIR-V translator the Plume/Vulkan backend uses
# (see the "Guest shaders on Vulkan/Plume" section below): glslang only, no
# spirv-cross, so it is a separate compilation unit rather than sharing
# rsx_shader_msl.cpp's dual-dependency guard.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp")

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
#  - spu_dispatch_mt.c: the musttail SPU indirect-branch fast path. It uses
#    __attribute__((musttail)), which MSVC cannot parse, and it must be compiled
#    *inside the game build* by clang so the tail call is guaranteed -- a plain
#    call leaks one resolver frame per guest loop iteration. A port adds it to
#    its own target; see templates/project/CMakeLists.txt.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/spu_dispatch_mt\.c$")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
else()
    # cellPad and cellAudio select their SDL2 backends unconditionally off
    # Windows (see PAD_BACKEND_SDL2 / AUDIO_BACKEND_SDL2), so SDL2 is required
    # here -- docs/BUILDING.md already lists it as the Linux/macOS dependency.
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        if(TARGET SDL2::SDL2)
            target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
        else()
            target_include_directories(ps3recomp_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(ps3recomp_runtime PRIVATE ${SDL2_LIBRARIES})
        endif()
        message(STATUS "  SDL2:         found (input + audio backends enabled)")
    else()
        message(FATAL_ERROR
            "SDL2 not found. It provides the cellPad and cellAudio backends on "
            "non-Windows hosts.\n"
            "  macOS:  brew install sdl2\n"
            "  Debian: apt install libsdl2-dev\n"
            "If SDL2 is installed in a non-standard prefix, pass "
            "-DCMAKE_PREFIX_PATH=<prefix>.")
    endif()

    find_package(Threads REQUIRED)
    target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)
endif()

if(APPLE)
    target_link_libraries(ps3recomp_runtime PUBLIC
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework AppKit"
    )
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_metal_backend.m"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "  Metal:        RSX backend enabled (rsx_metal_backend.m)")
endif()

# ---------------------------------------------------------------------------
# Guest shaders on Metal: HLSL -> SPIR-V -> MSL
# ---------------------------------------------------------------------------
# The RSX decompilers emit HLSL for the D3D12 backend. Metal cannot compile
# that, so libs/video/rsx_shader_msl.cpp lowers it with glslang (HLSL ->
# SPIR-V) and spirv-cross (SPIR-V -> MSL). Both are optional: without them the
# module compiles to a stub, rsx_hlsl_to_msl_available() says so, and the
# Metal backend stays on its fixed-function path. The translation has no GPU
# in it, so it is built and tested on Linux as well -- that is where the
# decompilers' HLSL meets glslang's front end on every push.
#
#   macOS:  brew install glslang spirv-cross
#   Debian: apt install glslang-dev glslang-tools spirv-tools \
#                       libspirv-cross-c-shared-dev
#           (glslang-tools only because the packaged CMake config declares
#           the standalone binary as an imported target and fails to load
#           without it)
set(PS3RECOMP_MSL_TRANSLATION OFF)
if(UNIX)
    find_package(glslang CONFIG QUIET)

    # spirv-cross's C API. Debian ships it as a shared library and Homebrew as
    # a static one, under different pkg-config names -- and pkg-config itself
    # is not a given on a Mac, so fall back to a plain header + library search.
    set(SPVC_OK OFF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SPVC QUIET spirv-cross-c-shared)
        if(NOT SPVC_FOUND)
            pkg_check_modules(SPVC QUIET spirv-cross-c)
        endif()
        if(SPVC_FOUND)
            set(SPVC_INC     ${SPVC_INCLUDE_DIRS})
            set(SPVC_LIBS    ${SPVC_LINK_LIBRARIES})
            set(SPVC_LIBDIRS ${SPVC_LIBRARY_DIRS})
            set(SPVC_OK ON)
        endif()
    endif()
    if(NOT SPVC_OK)
        find_path(SPVC_INC_DIR spirv_cross_c.h PATH_SUFFIXES spirv_cross)
        find_library(SPVC_LIB NAMES spirv-cross-c-shared spirv-cross-c)
        if(SPVC_INC_DIR AND SPVC_LIB)
            set(SPVC_INC  ${SPVC_INC_DIR})
            set(SPVC_LIBS ${SPVC_LIB})
            get_filename_component(SPVC_LIBDIRS "${SPVC_LIB}" DIRECTORY)
            set(SPVC_OK ON)
        endif()
    endif()

    if(glslang_FOUND AND SPVC_OK
       AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        add_library(ps3recomp_msl_translation INTERFACE)
        target_compile_definitions(ps3recomp_msl_translation INTERFACE
            PS3RECOMP_HAVE_MSL_TRANSLATION=1)
        target_include_directories(ps3recomp_msl_translation INTERFACE ${SPVC_INC})
        target_link_libraries(ps3recomp_msl_translation INTERFACE
            glslang::glslang glslang::SPIRV ${SPVC_LIBS})
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_msl_translation INTERFACE
                glslang::glslang-default-resource-limits)
        endif()
        # A static spirv-cross C API (Homebrew's) needs the backends it was
        # built from, and neither pkg-config nor the library name says so:
        # pick up the static siblings next to it. The shared one carries
        # its own.
        if(SPVC_LIBS MATCHES "\\.a$")
            foreach(part msl glsl hlsl cpp reflect util core)
                find_library(SPVC_PART_${part} NAMES spirv-cross-${part}
                    HINTS ${SPVC_LIBDIRS} NO_DEFAULT_PATH)
                if(SPVC_PART_${part})
                    target_link_libraries(ps3recomp_msl_translation INTERFACE
                        ${SPVC_PART_${part}})
                endif()
            endforeach()
        endif()
        target_link_libraries(ps3recomp_runtime PRIVATE ps3recomp_msl_translation)
        set(PS3RECOMP_MSL_TRANSLATION ON)
        message(STATUS "  Shaders:      guest programs translated to MSL (glslang ${glslang_VERSION}, ${SPVC_LIBS})")
    else()
        message(STATUS "  Shaders:      glslang/spirv-cross not found; Metal backend stays fixed-function")
    endif()

    # -----------------------------------------------------------------------
    # Guest shaders on Vulkan/Plume: HLSL -> SPIR-V
    # -----------------------------------------------------------------------
    # rsx_shader_spirv.cpp needs only glslang (not spirv-cross), so it is
    # enabled independently of PS3RECOMP_MSL_TRANSLATION above -- a machine
    # with glslang but not spirv-cross still gets a working Plume backend.
    if(glslang_FOUND AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        add_library(ps3recomp_spirv_translation INTERFACE)
        target_compile_definitions(ps3recomp_spirv_translation INTERFACE
            PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
        target_link_libraries(ps3recomp_spirv_translation INTERFACE
            glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_spirv_translation INTERFACE
                glslang::glslang-default-resource-limits)
        endif()
        target_link_libraries(ps3recomp_runtime PRIVATE ps3recomp_spirv_translation)
        set(PS3RECOMP_SPIRV_TRANSLATION ON)
        message(STATUS "  Shaders:      guest programs translatable to SPIR-V (glslang ${glslang_VERSION})")
    else()
        set(PS3RECOMP_SPIRV_TRANSLATION OFF)
        message(STATUS "  Shaders:      glslang not found; HLSL -> SPIR-V translation stubbed")
    endif()
endif()

# ---------------------------------------------------------------------------
# RSX Vulkan backend on Plume (Linux, and wherever else Plume reaches)
# ---------------------------------------------------------------------------
# See docs/RENDERER_MIGRATION.md: this is the cross-platform rsx_draw_backend
# implementation meant to retire the Windows-only D3D12 "live" path and the
# separate Metal decompiler over time, not a bolt-on beside them. It builds
# whenever a Plume checkout is found beside this repository (the same
# sibling-directory convention Xerenge's own rexgpu-plume uses) -- clone one
# with: git clone https://github.com/shipa-2/plume ../plume
# (that fork, not upstream renderbag/plume: this backend is written against
# SDL3, and upstream was still SDL2-only when it was written -- see the NOTE
# a few lines down.)
option(PS3RECOMP_RSX_BACKEND_PLUME "Build the Plume/Vulkan RSX draw backend" ${UNIX})
if(PS3RECOMP_RSX_BACKEND_PLUME AND NOT APPLE)
    set(PLUME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../plume")
    if(EXISTS "${PLUME_ROOT}/CMakeLists.txt")
        # NOTE: this project's Plume checkout (see PLUME_ROOT above) needs to
        # be one whose CMakeLists.txt links against SDL3 (find_package(SDL3)
        # / a PLUME_SDL3_TARGET, not SDL2) -- e.g. shipa-2/plume, not
        # upstream renderbag/plume at the time this backend was written,
        # which was still SDL2-only. rsx_draw_backend_plume.cpp is written
        # against the SDL3 API (SDL3/SDL.h, 4-argument SDL_CreateWindow,
        # SDL_EVENT_QUIT, bool-returning SDL_Init/SDL_PollEvent) to match.
        # Do NOT link SDL2 into this backend's own translation unit: mixing
        # SDL2 and SDL3 headers there is an ODR pileup (SDL_bool,
        # SDL_ScaleMode, SDL_CreateWindow's own argument count all differ
        # between the two), not something an include-path or link order fix
        # can paper over.
        #
        # Nor can ps3recomp_rsx_plume (SDL3) be linked into the SAME final
        # binary as ps3recomp_runtime's own SDL2 usage (cellPad/cellAudio):
        # CMake's own IMPORTED targets carry a COMPATIBLE_INTERFACE_STRING
        # SDL_VERSION property, and having both SDL2::SDL2 and an SDL3
        # target as transitive dependencies of one target is a hard CMake
        # generate-time error ("does not agree with the value of SDL_VERSION
        # already determined"), not just an ODR risk. This is exactly why
        # Xerenge's own Plume backend (rexgpu-plume) is a separate SHARED
        # library loaded as a plugin rather than statically linked into the
        # host executable -- its own comments say as much ("volk is confined
        # to this shared library so it cannot conflict"). Until this
        # backend gets the same plugin treatment (see docs/RENDERER_MIGRATION.md),
        # ps3recomp_rsx_plume is therefore its own standalone target: do NOT
        # link it into ps3recomp_runtime. A game that wants both cellPad/
        # cellAudio and this Plume/Vulkan backend in one process needs the
        # plugin split first.
        set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
        set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        add_subdirectory("${PLUME_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/plume")

        add_library(ps3recomp_rsx_plume STATIC
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/plume_renderer/rsx_draw_backend_plume.cpp"
            # rsx_shader_spirv.cpp needs only glslang, not SDL, so it's cheap
            # to compile a second time here rather than trying to share the
            # one already built into ps3recomp_runtime -- which this target
            # deliberately does NOT link against (see the NOTE above).
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp"
        )
        target_include_directories(ps3recomp_rsx_plume PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${PLUME_ROOT}"
        )
        # SDL3's own include dirs/libs come in transitively: `plume` links
        # its SDL3 target PUBLIC.
        target_link_libraries(ps3recomp_rsx_plume PUBLIC plume)
        if(TARGET ps3recomp_spirv_translation)
            target_link_libraries(ps3recomp_rsx_plume PRIVATE ps3recomp_spirv_translation)
        else()
            message(WARNING "  RSX backend:  glslang not found -- ps3recomp_rsx_plume's "
                            "pipeline_create() will always fail (rsx_hlsl_to_spirv stubbed out)")
        endif()
        message(STATUS "  RSX backend:  Plume/Vulkan enabled as its own target, ps3recomp_rsx_plume (${PLUME_ROOT}) -- NOT linked into ps3recomp_runtime, see the NOTE above")

        # Phase 1 smoke test: real window + device + swapchain + clear +
        # present, no pipelines/textures/draws yet. See
        # libs/video/tests/test_plume_backend_smoke.c and
        # docs/RENDERER_MIGRATION.md.
        add_executable(ps3recomp_plume_smoke
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/tests/test_plume_backend_smoke.c"
        )
        target_link_libraries(ps3recomp_plume_smoke PRIVATE ps3recomp_rsx_plume)
        message(STATUS "  RSX backend:  ps3recomp_plume_smoke target added")
    else()
        message(STATUS "  RSX backend:  Plume not found at ${PLUME_ROOT}; "
                      "clone an SDL3-linked Plume beside this repo to enable "
                      "(git clone https://github.com/shipa-2/plume ../plume; "
                      "upstream renderbag/plume was SDL2-only when this "
                      "backend was written against SDL3 -- see the NOTE "
                      "above)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. MSVC never contracts, and x86-64
        # without -mfma has nothing to contract into, so every validated
        # result came from separate multiplies and adds; arm64 has FMA in
        # the base ISA and Clang fuses by default there. A PPC fmadd is
        # spelt as one where the lifter means one.
        -ffp-contract=off
    )
elseif(MSVC)
    # Real cl.exe -- clang-cl reports its id as Clang and is handled above.
    #
    # This does not currently build. The runtime and the lifted output use GCC/
    # Clang builtins that cl has no equivalent for, and the failure lands as a
    # wall of errors in whichever file the build happens to reach first rather
    # than as anything that names the cause. Say it here instead.
    message(WARNING
        "Building with cl.exe (MSVC). This is not currently supported.\n"
        "  The runtime uses __atomic_* builtins, __int128 and __builtin_bswap*, "
        "and the lifted output uses __int128 for the PPC 64x64 multiplies. "
        "cl has none of them.\n"
        "  Use clang-cl instead -- it consumes the MSVC headers and libraries, "
        "so nothing else about a Visual Studio build changes:\n"
        "    cmake -S . -B build -G Ninja "
        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl\n"
        "  See docs/BUILDING.md. Porting the intrinsics to cl is a welcome "
        "contribution; nothing else stands in the way.")
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(NOT MSVC AND NOT WIN32)
    # dladdr/Dl_info (runtime/platform/win32_backtrace.h) are GNU extensions
    # that glibc hides unless _GNU_SOURCE is set. g++ defines it implicitly,
    # so the C++ boot scaffold never noticed; a C file including the same
    # shim does not get it for free.
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    # sync_stress is written directly against the Win32 threading API
    # (WaitForMultipleObjects, joinable HANDLEs, Interlocked*, CRITICAL_SECTION).
    # Off Windows all of that comes from runtime/platform/win32_compat.h, so
    # the suite runs everywhere -- and is the arm64 memory-model regression
    # net for the lv2 sync primitives.
    add_subdirectory(tests/sync_stress)
    # cellFiber switches contexts with Windows fibers on Windows and ucontext
    # everywhere else. Apple deprecated the ucontext routines and gates both
    # them and the shape of ucontext_t on _XOPEN_SOURCE, so whether that path
    # works on arm64 is a question to answer rather than assume.
    add_subdirectory(tests/fiber_switch)
endif()

# ---------------------------------------------------------------------------
# POSIX host harness
# ---------------------------------------------------------------------------
# Drives the cellGcm -> RSX -> backend bridge with no lifted game, so a backend
# can be brought up and regression-tested without a game binary or a recompiler
# run. Builds on any POSIX host: it picks Metal on Apple and the null backend's
# headless software path everywhere else, so Linux -- which has no real backend
# yet -- can still run the whole guest command stream end to end in CI.
if(UNIX)
    add_executable(ps3recomp_host
        runtime/host/host_posix.c
        runtime/host/host_stubs.c
    )
    target_link_libraries(ps3recomp_host PRIVATE ps3recomp_runtime)
    if(NOT WIN32)
        # glibc keeps the math functions in a separate library; MSVC does not.
        target_link_libraries(ps3recomp_host PRIVATE m)
    endif()
    target_include_directories(ps3recomp_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    # The runtime carries C++ (and, with translation on, spirv-cross), so the
    # C++ driver must do the final link or the standard library goes missing.
    set_target_properties(ps3recomp_host PROPERTIES LINKER_LANGUAGE CXX)

    # Guest shader translation, decompilers -> HLSL -> MSL, with no GPU: the
    # regression net for the Metal backend's shader path on every platform.
    if(PS3RECOMP_MSL_TRANSLATION)
        add_executable(test_shader_msl
            libs/video/tests/test_shader_msl.c
            libs/video/rsx_shader_msl.cpp
            libs/video/rsx_vp_decompiler.c
            libs/video/rsx_fp_decompiler.c
        )
        target_include_directories(test_shader_msl PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(test_shader_msl PRIVATE ps3recomp_msl_translation)
    endif()
endif()

# ---------------------------------------------------------------------------
# PPU boot smoke title
# ---------------------------------------------------------------------------
# The one target that LINKS AND RUNS the PPU boot scaffold. runtime/ppu/ is the
# game-agnostic boot path every port is built on, and nothing here has ever
# built it: it #includes the lifter's per-game ppu_recomp.h, so
# tools/check_ppu_scaffold.py compiles it against a stub header and stops
# there. Compiling is not running, and on POSIX the loader, the NID bridge, the
# lv2 syscalls, guest threads, the frame clock and the RSX backend had never
# once run together.
#
# runtime/ppu/tests/smoke/ supplies what was missing: a hand-written program in
# the lifter's ABI, and tools/make_smoke_elf.py writes both the PPU ELF it is
# loaded from and the ppu_recomp.h it compiles against -- the header from
# ppu_lifter.py's own HEADER_PREAMBLE, so the synthetic title cannot drift away
# from the ABI a real one gets.
#
# The HLE NID table is generated exactly as a port generates it, so the smoke
# title's firmware imports resolve through the same bridge a game's do.
if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "  Boot smoke:   no python3; ps3recomp_boot_smoke not built")
    else()
        set(SMOKE_GEN "${CMAKE_CURRENT_BINARY_DIR}/smoke")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/smoke.elf" "${SMOKE_GEN}/ppu_recomp.h"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    --out    "${SMOKE_GEN}/smoke.elf"
                    --header "${SMOKE_GEN}/ppu_recomp.h"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppu_lifter.py"
            COMMENT "Generating the smoke title's image and ppu_recomp.h")

        add_custom_command(
            OUTPUT "${SMOKE_GEN}/ppu_hle_nids.cpp"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    --out "${SMOKE_GEN}/ppu_hle_nids.cpp" --all
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nid_database.py"
            COMMENT "Generating the HLE NID -> handler table")

        add_executable(ps3recomp_boot_smoke
            runtime/ppu/tests/boot_main.cpp
            runtime/ppu/tests/smoke/smoke_recomp.c
            runtime/ppu/ppu_loader.cpp
            runtime/ppu/ppu_hle.cpp
            runtime/ppu/ppu_fs.cpp
            runtime/ppu/ppu_sysprx.cpp
            "${SMOKE_GEN}/ppu_hle_nids.cpp"
            "${SMOKE_GEN}/ppu_recomp.h"
        )
        target_include_directories(ps3recomp_boot_smoke PRIVATE
            "${SMOKE_GEN}"                                  # the generated ppu_recomp.h
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/memory"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/syscalls"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
        )
        target_link_libraries(ps3recomp_boot_smoke PRIVATE ps3recomp_runtime)
        set_target_properties(ps3recomp_boot_smoke PROPERTIES LINKER_LANGUAGE CXX)

        # Where the runnable image lands, so CI and a developer both say
        #   ./build/ps3recomp_boot_smoke build/smoke/smoke.elf
        message(STATUS "  Boot smoke:   ps3recomp_boot_smoke + smoke/smoke.elf")

        # The same lift again, arranged the way a port's own tree arranges one:
        # the generated ppu_recomp.h and the lifted source in one directory,
        # which is what templates/project wants for -DRECOMP_DIR. The header is
        # generated into the build tree and the source lives in the repository,
        # so a directory holding both has to be made rather than pointed at.
        #
        # This is what lets CI build the project template against a real lift
        # and run it -- the check that a port started from templates/project
        # still boots, on a platform where it can be run.
        set(SMOKE_LIFT "${SMOKE_GEN}/lift")
        set(SMOKE_LIFT_SRC
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu/tests/smoke/smoke_recomp.c")
        add_custom_command(
            OUTPUT "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SMOKE_LIFT}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT}/ppu_recomp.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SMOKE_LIFT_SRC}" "${SMOKE_LIFT}/smoke_recomp.c"
            DEPENDS "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT_SRC}"
            COMMENT "Staging the smoke title's lift as a RECOMP_DIR")
        add_custom_target(ps3recomp_smoke_lift ALL
            DEPENDS "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c")
        message(STATUS "  Smoke lift:   smoke/lift (a RECOMP_DIR for templates/project)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
