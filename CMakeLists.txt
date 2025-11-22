# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32c6")
set(ESPBAUD "115200")
set(ESPPORT "COM6")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(desk-satellite)
