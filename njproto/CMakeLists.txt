cmake_minimum_required(VERSION 3.16)
project(njproto CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

add_library(njproto STATIC
  src/message.cpp
  src/sha1.cpp
  src/connection.cpp
  src/client.cpp
  src/server.cpp
)
target_include_directories(njproto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(njserver tools/njserver.cpp)
target_link_libraries(njserver njproto)

add_executable(njcli tools/njcli.cpp)
target_link_libraries(njcli njproto)

enable_testing()
foreach(t test_codec test_sha1 test_loopback)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} njproto)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
