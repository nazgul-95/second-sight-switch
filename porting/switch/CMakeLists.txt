cmake_minimum_required(VERSION 3.20)
project(second_sight_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The local mksm-switch runtime checkout supplies the project's libnx/NRO
# backend. Its game-specific NFSU2 sources and generated code are not used.
set(XBOXRECOMP_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../external_versions/mksm-switch/xboxrecomp"
    CACHE PATH "Path to xboxrecomp with Switch support")
set(SECOND_SIGHT_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_boot" CACHE PATH
    "Directory containing output from tools.recomp")
option(SECOND_SIGHT_VULKAN "Use the experimental Switch NVK renderer" OFF)
option(SECOND_SIGHT_CPU_PROFILE "Enable the diagnostic 25 Hz CPU sampler" OFF)
option(SECOND_SIGHT_GFX_DIAGNOSTICS "Enable bounded per-mission NV2A draw-state capture" OFF)
option(SECOND_SIGHT_CPU_BOOST "Request the Switch CPU maximum while the game runs" OFF)
option(SECOND_SIGHT_FLOW_TRACE "Enable verbose generated frame bring-up traces" OFF)
option(SECOND_SIGHT_VK_UNIFORM_CACHE "Reuse immutable Vulkan uniform uploads" ON)
option(SECOND_SIGHT_PB_BULK "Batch complete vertex-program uploads" ON)
option(SECOND_SIGHT_VK_RAW_CMP "Unpack packed normals in Vulkan shaders" ON)
option(SECOND_SIGHT_VK_FEEDBACK_COPY "Copy render targets before self-sampling draws" ON)
option(SECOND_SIGHT_VK_RT_ALIASES "Sample cropped linear render-target aliases" ON)
option(SECOND_SIGHT_GAME_LTO "Experimental game/manual-only LTO (not NVK/Rust)" OFF)
option(SECOND_SIGHT_HW_PROGRESS "Bound guest poll loops on hardware progress" ON)
set(SECOND_SIGHT_LTO_JOBS "2" CACHE STRING "Parallel game LTO link jobs")

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Configure with devkitPro's Switch.cmake toolchain")
endif()
if(NOT EXISTS "${SECOND_SIGHT_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "Missing lifted C code in ${SECOND_SIGHT_GEN_DIR}")
endif()

set(XBOXRECOMP_VULKAN "${SECOND_SIGHT_VULKAN}" CACHE BOOL "" FORCE)
add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/xboxrecomp")

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${SECOND_SIGHT_GEN_DIR}/*.c")
set(SECOND_SIGHT_GAME_SOURCES src/recomp_manual.c ${RECOMP_SOURCES})
if(NOT SECOND_SIGHT_GAME_LTO)
    set(SECOND_SIGHT_DIRECT_MANUAL_SOURCE src/recomp_manual.c)
    set(SECOND_SIGHT_DIRECT_GAME_SOURCES ${RECOMP_SOURCES})
endif()
add_executable(second_sight_recomp
    src/main.c
    ${SECOND_SIGHT_DIRECT_MANUAL_SOURCE}
    src/boot_diagnostics.c
    src/crash_diagnostics.c
    src/profile_diagnostics.c
    src/keyboard_trace.c
    src/language_config.c
    src/frame_profile.c
    src/switch_clock.c
    ${SECOND_SIGHT_DIRECT_GAME_SOURCES}
)
target_include_directories(second_sight_recomp PRIVATE
    "${SECOND_SIGHT_GEN_DIR}"
    "${XBOXRECOMP_DIR}/include"
    "$ENV{DEVKITPRO}/portlibs/switch/include"
)
target_link_libraries(second_sight_recomp PRIVATE xboxrecomp nx m)
if(SECOND_SIGHT_HW_PROGRESS AND SECOND_SIGHT_VULKAN)
    target_compile_definitions(second_sight_recomp PRIVATE SECOND_SIGHT_HW_PROGRESS=1)
endif()
if(SECOND_SIGHT_FLOW_TRACE)
    target_compile_definitions(second_sight_recomp PRIVATE SECOND_SIGHT_FLOW_TRACE=1)
endif()
if(SECOND_SIGHT_VK_UNIFORM_CACHE)
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_VK_UNIFORM_CACHE=1)
endif()
if(SECOND_SIGHT_PB_BULK)
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_PB_BULK=1)
endif()
if(SECOND_SIGHT_VK_RAW_CMP)
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_VK_RAW_CMP=1)
endif()
if(SECOND_SIGHT_VK_FEEDBACK_COPY)
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_VK_FEEDBACK_COPY=1)
endif()
if(SECOND_SIGHT_VK_RT_ALIASES)
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_VK_RT_ALIASES=1)
endif()
if(SECOND_SIGHT_GAME_LTO)
    # The final NVK link uses LLD, which cannot consume GCC's LTO IR. First
    # partial-link just the game's C with GCC/BFD into a native ARM64 object.
    # No GCC IR or Rust LTO is passed across that boundary.
    add_library(second_sight_game_ir OBJECT ${SECOND_SIGHT_GAME_SOURCES})
    target_include_directories(second_sight_game_ir PRIVATE
        $<TARGET_PROPERTY:second_sight_recomp,INCLUDE_DIRECTORIES>)
    target_compile_definitions(second_sight_game_ir PRIVATE
        $<TARGET_PROPERTY:second_sight_recomp,COMPILE_DEFINITIONS>)
    target_compile_options(second_sight_game_ir PRIVATE
        -flto=auto -fno-strict-aliasing -fwrapv)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" SECOND_SIGHT_BUILD_TYPE_UPPER)
    separate_arguments(SECOND_SIGHT_PARTIAL_FLAGS UNIX_COMMAND
        "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${SECOND_SIGHT_BUILD_TYPE_UPPER}}")
    set(SECOND_SIGHT_NATIVE_GAME "${CMAKE_CURRENT_BINARY_DIR}/second_sight_game_native.o")
    add_custom_command(OUTPUT "${SECOND_SIGHT_NATIVE_GAME}"
        COMMAND "${CMAKE_C_COMPILER}" ${SECOND_SIGHT_PARTIAL_FLAGS}
            -nostdlib -r -fuse-ld=bfd "-flto=${SECOND_SIGHT_LTO_JOBS}"
            -flinker-output=nolto-rel -flto-partition=balanced
            -fno-strict-aliasing -fwrapv
            $<TARGET_OBJECTS:second_sight_game_ir> -o "${SECOND_SIGHT_NATIVE_GAME}"
        DEPENDS second_sight_game_ir $<TARGET_OBJECTS:second_sight_game_ir>
        COMMAND_EXPAND_LISTS VERBATIM
        COMMENT "Optimizing game C with GCC LTO into native ARM64")
    set_source_files_properties("${SECOND_SIGHT_NATIVE_GAME}" PROPERTIES
        GENERATED TRUE EXTERNAL_OBJECT TRUE)
    target_sources(second_sight_recomp PRIVATE "${SECOND_SIGHT_NATIVE_GAME}")
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_GAME_LTO=1)
endif()
if(SECOND_SIGHT_CPU_BOOST)
    set_property(SOURCE src/switch_clock.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_CPU_BOOST=1)
endif()
if(SECOND_SIGHT_CPU_PROFILE)
    # Only main.c consumes this launch switch; leave lifted objects unchanged.
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        SECOND_SIGHT_CPU_PROFILE=1)
endif()
if(SECOND_SIGHT_GFX_DIAGNOSTICS)
    target_compile_definitions(second_sight_recomp PRIVATE SECOND_SIGHT_GFX_DIAGNOSTICS=1)
endif()

if(SECOND_SIGHT_VULKAN)
    # Vulkan on Switch needs the separate mesa-switch NVK SDK. This option is
    # intentionally off until the basic OpenGL/NV2A path is brought up.
    target_compile_definitions(second_sight_recomp PRIVATE SECOND_SIGHT_VULKAN=1)
else()
    target_link_libraries(second_sight_recomp PRIVATE EGL glapi drm_nouveau)
endif()

set(SECOND_SIGHT_APP_NAME "Second Sight (Xbox recomp)" CACHE STRING
    "Homebrew title shown in hbmenu")
set(SECOND_SIGHT_AUTHOR "sp00nz" CACHE STRING "Homebrew author")
set(SECOND_SIGHT_VERSION "0.1.0" CACHE STRING "Homebrew version")
set(SECOND_SIGHT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg" CACHE FILEPATH
    "Optional homebrew icon")

set(SECOND_SIGHT_NACP "${CMAKE_CURRENT_BINARY_DIR}/second_sight.nacp")
nx_generate_nacp(OUTPUT "${SECOND_SIGHT_NACP}"
    NAME "${SECOND_SIGHT_APP_NAME}"
    AUTHOR "${SECOND_SIGHT_AUTHOR}"
    VERSION "${SECOND_SIGHT_VERSION}")
if(EXISTS "${SECOND_SIGHT_ICON}")
    nx_create_nro(second_sight_recomp NACP "${SECOND_SIGHT_NACP}"
        ICON "${SECOND_SIGHT_ICON}")
else()
    nx_create_nro(second_sight_recomp NACP "${SECOND_SIGHT_NACP}")
endif()

