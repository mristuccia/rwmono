cmake_minimum_required(VERSION 3.20)
project(rwmono CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw)

add_executable(rwmono
  src/main.cpp
  src/dng_writer.cpp
  src/ljpeg_encoder.cpp
)
target_link_libraries(rwmono PRIVATE PkgConfig::LIBRAW)
