cmake_minimum_required(VERSION 3.16)
project(RdmaRangeLock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# 当前状态：仅骨架，尚未有实现文件。
# 实现 v1 之后取消下面注释，并按版本逐个添加可执行目标。
#
# 目标命名约定：rrl_<版本号>_<短名>
#   rrl_v1_btree_single
#   rrl_v2_btree_latch
#   rrl_v3_record_2pl
#   rrl_v4_gap_lock
#   rrl_v5_next_key
#   rrl_v6_next_key_hier
# ---------------------------------------------------------------------------

# file(GLOB_RECURSE RRL_COMMON_SRC ${CMAKE_SOURCE_DIR}/src/common/*.cpp)
# add_library(rrl_common STATIC ${RRL_COMMON_SRC})
# target_include_directories(rrl_common PUBLIC ${CMAKE_SOURCE_DIR}/include)

# add_executable(rrl_v1_btree_single
#   ${CMAKE_SOURCE_DIR}/src/index/v1_btree_single.cpp
#   ${CMAKE_SOURCE_DIR}/benchmarks/runner.cpp)
# target_link_libraries(rrl_v1_btree_single PRIVATE rrl_common)

# enable_testing()
# add_subdirectory(test)
