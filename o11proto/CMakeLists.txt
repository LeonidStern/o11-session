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
find_package(Threads REQUIRED)                 # live::Transport's network thread
target_link_libraries(o11proto PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(o11proto PUBLIC ws2_32 bcrypt)
endif()

# The internal socket layer (src/socket_compat.h) is shared with o11server
# and the loopback test, which also touch sockets directly.
add_executable(o11server tools/o11server.cpp)
target_include_directories(o11server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(o11server o11proto)

add_executable(o11cli tools/o11cli.cpp)
target_link_libraries(o11cli o11proto)

enable_testing()
foreach(t test_codec test_sha1 test_loopback)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${t} o11proto)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Live mode (PROTOCOL.md 15): the relay's conformance tests and the client transport.
foreach(t test_live_server test_live_transport test_live_room)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${t} o11proto)
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
            test_engine_live_paths)
    add_executable(${t} engine/tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                            ${CMAKE_CURRENT_SOURCE_DIR}/engine/src)
    target_link_libraries(${t} o11proto_engine)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 300)
  endforeach()
  # The tone test's loopback probe sends with udp_compat.h.
  target_include_directories(test_engine_live_tone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_tests_properties(test_engine_live_room PROPERTIES TIMEOUT 900)
  set_tests_properties(test_engine_live_tone PROPERTIES TIMEOUT 480)   # 7 runs of 12-20 s
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
  target_include_directories(o11proto_portmap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(o11proto_portmap PUBLIC Threads::Threads PRIVATE miniupnpc natpmp)
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
    target_link_libraries(test_portmap o11proto_portmap o11proto)
    add_test(NAME test_portmap COMMAND test_portmap)
    set_tests_properties(test_portmap PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 120)
  else()
    message(STATUS "python3 not found: test_portmap is not built")
  endif()
endif()
