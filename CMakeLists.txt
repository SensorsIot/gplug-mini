# ESP-IDF 6.x requires 3.22; a 3.16 line copied from a 5.x template fails to
# configure, and the error is easy to miss in CMake output.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Must match APP_NAME in .github/workflows/build.yml — it names the .bin.
project(gplug-mini)

# dlms_parser v2.1.0 does not compile clean under ESP-IDF 6 on riscv32: it
# formats uint32_t with %u, which is right on a platform where int is 32 bits
# and wrong here, where uint32_t is `long unsigned int`. IDF builds with
# -Werror, so three warnings inside the library stop our build.
#
# Suppressed for that component alone, never project-wide: the check is a real
# one and our own code should keep failing on it. Remove when upstream fixes the
# formats; a suppression left on one component after that costs only a line.
idf_component_get_property(dlms_lib esphome__dlms_parser COMPONENT_LIB)
if(dlms_lib)
    target_compile_options(${dlms_lib} PRIVATE -Wno-error=format)
endif()
