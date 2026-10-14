cmake_minimum_required(VERSION 3.21)
project(bbcenter_ext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if (MSVC)
  add_compile_options(/utf-8 /EHsc /W3)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(bbcenter_ext SHARED src/ext.cpp)
target_link_libraries(bbcenter_ext PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(bbcenter_ext PRIVATE BBEXT_VER="0.13-poc")

add_executable(BBCenterLoader src/loader.cpp)
target_compile_definitions(BBCenterLoader PRIVATE BBEXT_VER="0.13-poc")
