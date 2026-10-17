cmake_minimum_required(VERSION 3.16)
project(njproto CXX)

set(CMAKE_CXX_STANDARD 17)
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

add_library(njproto STATIC
  src/message.cpp
  src/sha1.cpp
  src/connection.cpp
  src/client.cpp
  src/server.cpp
)
target_include_directories(njproto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
  target_link_libraries(njproto PUBLIC ws2_32)
endif()

# The internal socket layer (src/socket_compat.h) is shared with njserver
# and the loopback test, which also touch sockets directly.
add_executable(njserver tools/njserver.cpp)
target_include_directories(njserver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(njserver njproto)

add_executable(njcli tools/njcli.cpp)
target_link_libraries(njcli njproto)

enable_testing()
foreach(t test_codec test_sha1 test_loopback)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${t} njproto)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# ---------------------------------------------------------------- engine
# The client engine (ENGINE.md) needs libopus and libFLAC.
option(NJPROTO_BUILD_ENGINE "Build the njproto_engine library, njjam and its tests" ON)
if(NJPROTO_BUILD_ENGINE)
  add_library(njproto_engine STATIC
    engine/src/dsp.cpp
    engine/src/opus_codec.cpp
    engine/src/flac_codec.cpp
    engine/src/engine.cpp
  )
  target_include_directories(njproto_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  # A parent project that builds the codecs itself (the Jamsa plug-in) defines the targets
  # opus and FLAC before adding this directory; otherwise the system packages are used.
  if(TARGET opus AND TARGET FLAC)
    target_link_libraries(njproto_engine PUBLIC njproto opus FLAC)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CODECS REQUIRED IMPORTED_TARGET opus flac)
    target_link_libraries(njproto_engine PUBLIC njproto PkgConfig::CODECS)
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(njproto_engine PUBLIC Threads::Threads)

  add_executable(njjam tools/njjam.cpp)
  target_link_libraries(njjam njproto_engine)

  foreach(t test_engine_codecs test_engine_clock test_engine_e2e)
    add_executable(${t} engine/tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${t} njproto_engine)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
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
  set(NJPROTO_PORTMAP_DEFAULT ON)
else()
  set(NJPROTO_PORTMAP_DEFAULT OFF)
  message(STATUS "miniupnpc or libnatpmp not found: njproto_portmap is off (NJPROTO_BUILD_PORTMAP)")
endif()
option(NJPROTO_BUILD_PORTMAP "Build the njproto_portmap library, its test and njserver --port-map" ${NJPROTO_PORTMAP_DEFAULT})
if(NJPROTO_BUILD_PORTMAP)
  if(NOT TARGET miniupnpc OR NOT TARGET natpmp)
    message(FATAL_ERROR "NJPROTO_BUILD_PORTMAP needs miniupnpc and libnatpmp")
  endif()
  find_package(Threads REQUIRED)
  add_library(njproto_portmap STATIC portmap/src/portmap.cpp)
  target_include_directories(njproto_portmap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(njproto_portmap PUBLIC Threads::Threads PRIVATE miniupnpc natpmp)
  if(WIN32)
    target_link_libraries(njproto_portmap PRIVATE ws2_32 iphlpapi)
  endif()

  target_compile_definitions(njserver PRIVATE NJPROTO_HAVE_PORTMAP=1)
  target_link_libraries(njserver njproto_portmap)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_executable(test_portmap portmap/tests/test_portmap.cpp)
    target_include_directories(test_portmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_portmap PRIVATE
      "FAKE_ROUTER_PY=\"${CMAKE_CURRENT_SOURCE_DIR}/portmap/tests/fake_router.py\""
      "PYTHON3_EXE=\"${Python3_EXECUTABLE}\"")
    target_link_libraries(test_portmap njproto_portmap njproto)
    add_test(NAME test_portmap COMMAND test_portmap)
    set_tests_properties(test_portmap PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 120)
  else()
    message(STATUS "python3 not found: test_portmap is not built")
  endif()
endif()
