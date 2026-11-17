cmake_minimum_required(VERSION 3.25...4.2)

project(cache_analysis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE)

add_executable(cache_analysis main.cpp)
target_compile_options(cache_analysis PRIVATE -Wall -Wextra)
