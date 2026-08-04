# Host-tier tests. Plain compiler, no ESP-IDF, no hardware.
#
#   cmake -S test/host -B build/host && cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
#
# dlms_parser is fetched at the same version the firmware pins (FR-DEC-01), so a
# host run and a device run exercise the same decoder. Pinned to a tag, not a
# branch: these tests exist to notice when the library's behaviour changes, which
# they cannot do if the library moves underneath them.
cmake_minimum_required(VERSION 3.22)
project(gplug_mini_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  dlms_parser
  GIT_REPOSITORY https://github.com/esphome-libs/dlms_parser.git
  GIT_TAG        v2.1.0
  GIT_SHALLOW    TRUE
)
# Its own test suite is not ours to run.
set(DLMS_PARSER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dlms_parser)
set(BUILD_TESTING ON)

# Link the library's own target rather than re-globbing its sources: building it
# twice would let the test drift from what the library actually ships.
add_executable(test_decode test_decode.cpp)
target_link_libraries(test_decode PRIVATE dlms_parser)
target_include_directories(test_decode PRIVATE ${dlms_parser_SOURCE_DIR}/src)
target_compile_options(test_decode PRIVATE -Wall -Wextra)

enable_testing()
set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

# One CTest entry per case, so a failure names the property that broke rather
# than "the decode test".
add_test(NAME TS-HOST-01a.serial-16-chars
         COMMAND test_decode serial16 ${FIXTURES}/e450_serial16.hex)
add_test(NAME TS-HOST-01b.serial-8-chars
         COMMAND test_decode serial8 ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-HOST-02.block-reassembly
         COMMAND test_decode reassembly ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-HOST-03.scaling-applied-once
         COMMAND test_decode scaling ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-HOST-04.energy-needs-integers
         COMMAND test_decode energy ${FIXTURES}/e450_serial8.hex)
