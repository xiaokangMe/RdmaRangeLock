cmake_minimum_required(VERSION 3.16)
project(RdmaRangeLock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(rrl_lock STATIC
  src/lock/lock_manager.cpp
)
target_include_directories(rrl_lock PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(rrl_lock PRIVATE -Wall -Wextra -Wpedantic)

add_executable(rrl_m1_range_lock_test
  test/m1_range_lock_test.cpp
)
target_link_libraries(rrl_m1_range_lock_test PRIVATE rrl_lock)
target_compile_options(rrl_m1_range_lock_test PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME rrl_m1_range_lock_test COMMAND rrl_m1_range_lock_test)