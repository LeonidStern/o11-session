cmake_minimum_required(VERSION 3.16)
project(o11proto CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()
if(WIN32)
  # Winsock2 needs the lean windows.h; NOMINMAX keeps std::min/max usable;
  # _CRT_SECURE_NO_WARNINGS silences MSVC over strerror/fopen/strtol;
  # _USE_MATH_DEFINES gives M_PI from <cmath>.
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()

# ------------------------------------------------------------------ Asio
# Standalone Asio (no Boost), header-only, under the Boost Software License 1.0
# (third_party/Asio-LICENSE_1_0.txt): the sockets, waits and timers under the
# client, the server and Live's transport. It is downloaded at configure time
# into the build tree and used from there; nothing is installed. To use a local
# copy of the same release pass -DFETCHCONTENT_SOURCE_DIR_ASIO=/path/to/asio-1.38.2.
# 1.38.2 is also the release Ableton Link 4.1 bundles, but each keeps its own:
# ours is in the inline namespace asio::o11_asio_1_38_2, Link's in its own, so
# the two never share a symbol in the plug-in. Asio stays inside the libraries
# (no public header includes it).
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)                # extracted files get the time of extraction
endif()
FetchContent_Declare(asio
  URL https://downloads.sourceforge.net/project/asio/asio/1.38.2%20%28Stable%29/asio-1.38.2.tar.bz2
  URL_HASH SHA256=c04e0e66ac29741faad763a56f3c50196421d4b968009fc237c53314769bf8ad
)
FetchContent_MakeAvailable(asio)               # headers only: the release has no CMake build
find_package(Threads REQUIRED)
add_library(o11proto_asio INTERFACE)
target_include_directories(o11proto_asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/include)
target_compile_definitions(o11proto_asio INTERFACE
  ASIO_STANDALONE=1
  ASIO_NO_DEPRECATED=1
  ASIO_DISABLE_VISIBILITY=1                    # its symbols stay as visible as ours, no more
  ASIO_VERSION_NAMESPACE=o11_asio_1_38_2)
target_link_libraries(o11proto_asio INTERFACE Threads::Threads)
if(WIN32)
  # Asio wants the Windows version named; this is the SDK's own default, which
  # o11proto's sources were built with before (Asio's calls need Windows 7).
  # Without I/O completion ports Asio waits with select() in the thread that
  # runs the io_context, as the old loops did with WSAPoll(): with them, it
  # would wait in threads of its own, at their ordinary priority, and hand the
  # real-time network thread what came.
  target_compile_definitions(o11proto_asio INTERFACE _WIN32_WINNT=0x0A00 ASIO_DISABLE_IOCP=1)
  target_link_libraries(o11proto_asio INTERFACE ws2_32 mswsock)
endif()

add_library(o11proto STATIC
  src/message.cpp
  src/sha1.cpp
  src/connection.cpp
  src/client.cpp
  src/server.cpp
  src/server_live.cpp
  src/live.cpp
  src/live_room.cpp
  src/live_transport.cpp
  src/realtime.cpp
)
target_include_directories(o11proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(o11proto PUBLIC Threads::Threads PRIVATE o11proto_asio)   # live::Transport's network thread
if(WIN32)
  target_link_libraries(o11proto PUBLIC ws2_32 bcrypt winmm)   # winmm: timeBeginPeriod() (realtime.cpp)
endif()

# The internal socket layer (src/socket_compat.h) is shared with o11server
# and the loopback test, which also touch sockets directly.
add_executable(o11server tools/o11server.cpp)
target_include_directories(o11server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(o11server o11proto o11proto_asio)

add_executable(o11cli tools/o11cli.cpp)
target_link_libraries(o11cli o11proto)

enable_testing()
foreach(t test_codec test_sha1 test_loopback)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${t} o11proto o11proto_asio)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Live mode (PROTOCOL.md 15): the relay's conformance tests and the client transport.
foreach(t test_live_server test_live_transport test_live_room)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${t} o11proto o11proto_asio)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES TIMEOUT 120)
endforeach()

# ---------------------------------------------------------------- engine
# The client engine (ENGINE.md) needs libopus and libFLAC.
option(O11PROTO_BUILD_ENGINE "Build the o11proto_engine library, o11jam and its tests" ON)
if(O11PROTO_BUILD_ENGINE)
  add_library(o11proto_engine STATIC
    engine/src/dsp.cpp
    engine/src/opus_codec.cpp
    engine/src/flac_codec.cpp
    engine/src/engine.cpp
    engine/src/engine_live.cpp
    engine/src/live_layer.cpp
    engine/src/live_sink.cpp
    engine/src/live_aoo.cpp
  )
  target_include_directories(o11proto_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  # A parent project that builds the codecs itself (the O11 Session plug-in) defines the targets
  # opus and FLAC before adding this directory; otherwise the system packages are used.
  if(TARGET opus AND TARGET FLAC)
    target_link_libraries(o11proto_engine PUBLIC o11proto opus FLAC)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CODECS REQUIRED IMPORTED_TARGET opus flac)
    target_link_libraries(o11proto_engine PUBLIC o11proto PkgConfig::CODECS)
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(o11proto_engine PUBLIC Threads::Threads)

  add_executable(o11jam tools/o11jam.cpp)
  target_link_libraries(o11jam o11proto_engine)
  add_executable(o11bench tools/o11bench.cpp)
  target_link_libraries(o11bench o11proto_engine)

  foreach(t test_engine_codecs test_engine_clock test_engine_e2e)
    add_executable(${t} engine/tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${t} o11proto_engine)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  # Live mode (ENGINE.md section 16): the unit tests reach the engine's internal headers.
  foreach(t test_engine_live_units test_engine_live test_engine_live_room test_engine_live_tone
            test_engine_live_paths test_engine_live_stalls)
    add_executable(${t} engine/tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                            ${CMAKE_CURRENT_SOURCE_DIR}/engine/src)
    target_link_libraries(${t} o11proto_engine)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 300)
  endforeach()
  # The tone test's loopback probe sends with udp_compat.h.
  target_include_directories(test_engine_live_tone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_engine_live_tone o11proto_asio)
  set_tests_properties(test_engine_live_room PROPERTIES TIMEOUT 900)
  set_tests_properties(test_engine_live_tone PROPERTIES TIMEOUT 480)   # 7 runs of 12-20 s
  set_tests_properties(test_engine_live_stalls PROPERTIES TIMEOUT 480) # 22 rooms of 8-9 s
endif()

# --------------------------------------------------------------- portmap
# The router port mapper (PORTMAP.md) needs miniupnpc and libnatpmp. A parent
# project may define the targets miniupnpc and natpmp itself; otherwise
# miniupnpc comes from pkg-config and libnatpmp (which ships no .pc file) is
# looked up by header and library name.
if(NOT TARGET miniupnpc)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MINIUPNPC QUIET IMPORTED_TARGET miniupnpc)
    if(MINIUPNPC_FOUND)
      add_library(miniupnpc INTERFACE)
      target_link_libraries(miniupnpc INTERFACE PkgConfig::MINIUPNPC)
    endif()
  endif()
endif()
if(NOT TARGET natpmp)
  find_path(NATPMP_INCLUDE_DIR natpmp.h)
  find_library(NATPMP_LIBRARY NAMES natpmp)
  if(NATPMP_INCLUDE_DIR AND NATPMP_LIBRARY)
    add_library(natpmp INTERFACE)
    target_include_directories(natpmp INTERFACE ${NATPMP_INCLUDE_DIR})
    target_link_libraries(natpmp INTERFACE ${NATPMP_LIBRARY})
  endif()
endif()
if(TARGET miniupnpc AND TARGET natpmp)
  set(O11PROTO_PORTMAP_DEFAULT ON)
else()
  set(O11PROTO_PORTMAP_DEFAULT OFF)
  message(STATUS "miniupnpc or libnatpmp not found: o11proto_portmap is off (O11PROTO_BUILD_PORTMAP)")
endif()
option(O11PROTO_BUILD_PORTMAP "Build the o11proto_portmap library, its test and o11server --port-map" ${O11PROTO_PORTMAP_DEFAULT})
if(O11PROTO_BUILD_PORTMAP)
  if(NOT TARGET miniupnpc OR NOT TARGET natpmp)
    message(FATAL_ERROR "O11PROTO_BUILD_PORTMAP needs miniupnpc and libnatpmp")
  endif()
  find_package(Threads REQUIRED)
  add_library(o11proto_portmap STATIC portmap/src/portmap.cpp)
  target_include_directories(o11proto_portmap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                              PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(o11proto_portmap PUBLIC Threads::Threads PRIVATE miniupnpc natpmp o11proto_asio)
  if(WIN32)
    target_link_libraries(o11proto_portmap PRIVATE ws2_32 iphlpapi)
  endif()

  target_compile_definitions(o11server PRIVATE O11PROTO_HAVE_PORTMAP=1)
  target_link_libraries(o11server o11proto_portmap)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_executable(test_portmap portmap/tests/test_portmap.cpp)
    target_include_directories(test_portmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_portmap PRIVATE
      "FAKE_ROUTER_PY=\"${CMAKE_CURRENT_SOURCE_DIR}/portmap/tests/fake_router.py\""
      "PYTHON3_EXE=\"${Python3_EXECUTABLE}\"")
    target_link_libraries(test_portmap o11proto_portmap o11proto o11proto_asio)
    add_test(NAME test_portmap COMMAND test_portmap)
    set_tests_properties(test_portmap PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 120)
  else()
    message(STATUS "python3 not found: test_portmap is not built")
  endif()
endif()
