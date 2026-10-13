cmake_minimum_required(VERSION 3.16)
project(algo_risk_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ARG_SANITIZE "Build with ASan + UBSan (GCC/Clang)" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
  endif()
  if(ARG_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

include_directories(include)

add_executable(arg_sim src/sim_main.cpp)
add_executable(arg_replay src/replay_main.cpp)
add_executable(arg_recon src/recon_main.cpp)
add_executable(arg_bench bench/arg_bench.cpp)
add_executable(arg_tests tests/arg_tests.cpp)

enable_testing()
add_test(NAME arg_tests COMMAND arg_tests --ops 1000000)
