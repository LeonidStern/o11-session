# O11 Session: a NINJAM-style jam plug-in for Ableton Live, built on JUCE over the clean-room
# o11proto engine (../o11proto). VST3 only.
#
#   cmake -S plugin -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# Everything it needs is downloaded at configure time and linked statically: JUCE 8.0.9
# (AGPLv3 or commercial: see README.md), Opus and FLAC (BSD), miniupnpc and libnatpmp (BSD,
# for the built-in server's port mapping) and Ableton Link (GPL v2 or later, header-only, with
# its asio-standalone; so the plug-in built with it is GPL). To use local copies pass
# -DFETCHCONTENT_SOURCE_DIR_<NAME>=/path for JUCE, OPUS, FLAC, MINIUPNP, LIBNATPMP or LINK
# (with its modules/asio-standalone).
#
# On Windows build with the static runtime (-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
# -DCMAKE_POLICY_DEFAULT_CMP0091=NEW) so O11Session.vst3 needs no Visual C++ runtime.
#
# Targets: o11_core (the engine wrapper, the built-in server, host sync and Link, click and the editor's texts: a static
# library on juce_core and juce_audio_basics only, which the tests link without a host),
# O11 Session (the plug-in: processor and editor) and the tests under tests/ (O11_TESTS=ON).

cmake_minimum_required(VERSION 3.25)

project(o11session VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

option(O11_TESTS "Build the tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Codecs: Opus and FLAC (static), as ninjam/vst3 builds them (no Ogg: the engine's Opus
# framing is its own)
# ---------------------------------------------------------------------------
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG 1.4.3
    GIT_SHALLOW TRUE
)

# A function keeps these settings out of the rest of the build.
function(o11_fetch_codecs)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # let option() see the normal variables below
    set(CMAKE_POLICY_DEFAULT_CMP0063 NEW) # hidden symbols in the static libraries too
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_FRAMEWORK OFF)
    set(INSTALL_DOCS OFF)
    set(INSTALL_PKG_CONFIG_MODULE OFF)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF)

    set(OPUS_BUILD_SHARED_LIBRARY OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
    list(LENGTH CMAKE_OSX_ARCHITECTURES num_archs)
    if(num_archs GREATER 1)
        # a universal build compiles each file for both CPUs, so no CPU specific code
        set(OPUS_DISABLE_INTRINSICS ON)
    endif()
    FetchContent_MakeAvailable(opus)
    # the engine includes <opus/opus.h>, as with the system package
    file(COPY ${opus_SOURCE_DIR}/include/ DESTINATION ${CMAKE_BINARY_DIR}/opus-include/opus)
    target_include_directories(opus INTERFACE ${CMAKE_BINARY_DIR}/opus-include)

    set(BUILD_CXXLIBS OFF)
    set(BUILD_PROGRAMS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(INSTALL_MANPAGES OFF)
    set(INSTALL_PKGCONFIG_MODULES OFF)
    set(INSTALL_CMAKE_CONFIG_MODULE OFF)
    set(WITH_OGG OFF) # NINJAM sends native FLAC
    set(WITH_STACK_PROTECTOR OFF)
    FetchContent_MakeAvailable(flac)
endfunction()

o11_fetch_codecs()

# ---------------------------------------------------------------------------
# Router port mapping: miniupnpc (UPnP-IGD) and libnatpmp (NAT-PMP), static, as ninjam/vst3
# builds them. o11proto's port mapper (o11proto_portmap) takes them as the targets miniupnpc and
# natpmp when those exist before it is added, so they are made here.
# ---------------------------------------------------------------------------
FetchContent_Declare(miniupnp
    GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
    GIT_TAG miniupnpc_2_2_6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR miniupnpc
)
# libnatpmp has no tags: this is master on 2025-04-03 (20230423 plus the CMake fixes after it)
FetchContent_Declare(libnatpmp
    GIT_REPOSITORY https://github.com/miniupnp/libnatpmp.git
    GIT_TAG 134fc89e2781e154e40042641f4d8bcbe42579f1
    GIT_SHALLOW TRUE
)

function(o11_fetch_portmap_libs)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)

    set(UPNPC_BUILD_STATIC ON)
    set(UPNPC_BUILD_SHARED OFF)
    set(UPNPC_BUILD_TESTS OFF)
    set(UPNPC_BUILD_SAMPLE OFF)
    set(UPNPC_NO_INSTALL ON)
    FetchContent_MakeAvailable(miniupnp)
    set_target_properties(libminiupnpc-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # The port mapper includes <miniupnpc/miniupnpc.h> and <natpmp.h>, as with the system
    # packages, so the headers go to one include folder in the build tree. libnatpmp's own
    # folder is kept off the include path: its file VERSION would be found by <version> on a
    # case-insensitive file system.
    set(inc ${CMAKE_BINARY_DIR}/portmap-include)
    file(COPY ${miniupnp_SOURCE_DIR}/miniupnpc/include/ DESTINATION ${inc}/miniupnpc)
    add_library(miniupnpc INTERFACE)
    target_include_directories(miniupnpc INTERFACE ${inc})
    target_link_libraries(miniupnpc INTERFACE libminiupnpc-static)

    FetchContent_MakeAvailable(libnatpmp) # the static library target natpmp
    set_target_properties(natpmp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    file(COPY ${libnatpmp_SOURCE_DIR}/natpmp.h ${libnatpmp_SOURCE_DIR}/natpmp_declspec.h DESTINATION ${inc})
    set_target_properties(natpmp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${inc})
    # its command line tools aren't needed
    foreach(t natpmpc testgetgateway)
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
endfunction()

o11_fetch_portmap_libs()

# ---------------------------------------------------------------------------
# JUCE
# ---------------------------------------------------------------------------
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(juce)

# ---------------------------------------------------------------------------
# Ableton Link (header-only): while Sync is on, Ableton Live (with its Link on) follows the
# room's tempo, and its bars start with the intervals (source/linksync.cpp)
# ---------------------------------------------------------------------------
FetchContent_Declare(link
    GIT_REPOSITORY https://github.com/Ableton/link.git
    GIT_TAG Link-4.1
    GIT_SHALLOW TRUE
    GIT_SUBMODULES modules/asio-standalone
    SOURCE_SUBDIR include # only lists Link's headers: without Link's examples and tests
)
FetchContent_MakeAvailable(link)
# AbletonLinkConfig.cmake defines the Ableton::Link target (include paths, platform and asio
# definitions). It also sets CMAKE_CXX_STANDARD, which a function keeps to itself.
function(o11_link_target)
    include(${link_SOURCE_DIR}/AbletonLinkConfig.cmake)
endfunction()
o11_link_target()
if(WIN32)
    # Link's network code (its #pragma comment(lib) covers MSVC only)
    set_property(TARGET Ableton::Link APPEND PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 iphlpapi)
endif()

# ---------------------------------------------------------------------------
# The o11proto protocol library, client engine and port mapper, with the libraries above
# ---------------------------------------------------------------------------
set(O11PROTO_BUILD_ENGINE ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../o11proto ${CMAKE_CURRENT_BINARY_DIR}/o11proto)
if(NOT TARGET o11proto_portmap)
    message(FATAL_ERROR "o11proto's port mapper (O11PROTO_BUILD_PORTMAP) is off; the built-in server needs it")
endif()

# the warnings our own sources are built with (the plug-in must build without any), and on
# Windows the lean windows.h without its min and max macros (std::min and std::max are used)
function(o11_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# o11_core: everything but the processor and the editor, for the tests too
# ---------------------------------------------------------------------------
add_library(o11_core STATIC
    source/engine.cpp
    source/engine.h
    source/hostserver.cpp
    source/hostserver.h
    source/hostsync.cpp
    source/hostsync.h
    source/linksync.cpp
    source/linksync.h
    source/click.cpp
    source/click.h
    source/uitext.cpp
    source/uitext.h
    source/version.h
)
target_include_directories(o11_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(o11_core
    PUBLIC
        O11_VERSION="${PROJECT_VERSION}"
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_MODAL_LOOPS_PERMITTED=0
)
# The JUCE modules are compiled into this library (PRIVATE), and again into the plug-in's
# shared code; a static library's copy is only used where the plug-in's isn't, so the two
# don't clash. The headers and definitions are passed on for the tests.
target_link_libraries(o11_core
    PRIVATE juce::juce_audio_basics juce::juce_core Ableton::Link
    PUBLIC o11proto_engine o11proto o11proto_portmap juce::juce_recommended_config_flags
)
o11_warnings(o11_core)
# asio makes some of its symbols visible to other modules unless told not to: this, and hidden
# symbols for the library, keep Link's and asio's code inside the plug-in, apart from the host's
# Link (Live's) and other plug-ins'
target_compile_definitions(o11_core PRIVATE ASIO_DISABLE_VISIBILITY=1)
set_target_properties(o11_core PROPERTIES CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN TRUE)
target_include_directories(o11_core PUBLIC $<TARGET_PROPERTY:juce::juce_audio_basics,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(o11_core PUBLIC $<TARGET_PROPERTY:juce::juce_audio_basics,INTERFACE_COMPILE_DEFINITIONS>)
if(WIN32)
    target_link_libraries(o11_core PUBLIC ws2_32 secur32 netapi32)
endif()

# ---------------------------------------------------------------------------
# The plug-in
# ---------------------------------------------------------------------------
juce_add_plugin(O11Session
    PRODUCT_NAME "O11Session"
    PLUGIN_NAME "O11 Session"
    COMPANY_NAME "O11 Session"
    BUNDLE_ID io.github.leonidstern.o11session
    PLUGIN_MANUFACTURER_CODE O11s
    PLUGIN_CODE Sesn
    FORMATS VST3
    VST3_CATEGORIES Fx Network
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    HARDENED_RUNTIME_ENABLED TRUE
    VST3_AUTO_MANIFEST FALSE
)
target_sources(O11Session PRIVATE
    source/processor.cpp
    source/processor.h
    source/editor.cpp
    source/editor.h
    source/views.cpp
    source/views.h
)
# Inter (SIL Open Font License 1.1, fonts/Inter-LICENSE.txt), the editor's typeface, built in
juce_add_binary_data(o11_fonts
    HEADER_NAME O11Fonts.h
    NAMESPACE O11Fonts
    SOURCES fonts/Inter-Regular.ttf fonts/Inter-SemiBold.ttf
)
target_compile_definitions(O11Session PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(O11Session
    PRIVATE o11_core o11_fonts juce::juce_audio_processors juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
)
o11_warnings(O11Session)

if(O11_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
