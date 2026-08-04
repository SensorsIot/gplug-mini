# ESP-IDF 6.x requires 3.22; a 3.16 line copied from a 5.x template fails to
# configure, and the error is easy to miss in CMake output.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Must match APP_NAME in .github/workflows/build.yml — it names the .bin.
project(gplug-mini)
