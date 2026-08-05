# Host-tier tests. Plain compiler, no ESP-IDF, no hardware.
#
#   cmake -S testing/host -B build/host && cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
#
# dlms_parser is fetched at the same version main/idf_component.yml pins, so a
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
target_include_directories(test_decode PRIVATE ${dlms_parser_SOURCE_DIR}/src
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../../main)
target_compile_options(test_decode PRIVATE -Wall -Wextra)

# The pure cores, compiled from main/ — the firmware's own source, not a copy.
# A host suite that tests a reimplementation cannot fail usefully.
add_executable(test_cores test_cores.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../main/obis_map.cpp
                          ${CMAKE_CURRENT_SOURCE_DIR}/../../main/ha_discovery.cpp)
target_include_directories(test_cores PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../main)
target_compile_options(test_cores PRIVATE -Wall -Wextra)
# config.h declares its storage functions behind this, because their signatures
# need ESP-IDF headers that do not exist here. The record and the rules that
# judge it are what the host tests want, and those need nothing.
target_compile_definitions(test_cores PRIVATE GPLUG_HOST_TEST)

enable_testing()
set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

# One CTest entry per case, so a failure names the property that broke rather
# than "the decode test".
add_test(NAME TS-001.serial-16-chars
         COMMAND test_decode serial16 ${FIXTURES}/e450_serial16.hex)
add_test(NAME TS-002.serial-8-chars
         COMMAND test_decode serial8 ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-003.block-reassembly
         COMMAND test_decode reassembly ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-004.scaling-applied-once
         COMMAND test_decode scaling ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-005.energy-needs-integers
         COMMAND test_decode energy ${FIXTURES}/e450_serial8.hex)
# FR-MTR-06. Two offsets, because they lose different things: 17 bytes in is
# inside the first frame's payload, 200 in has lost that frame entirely.
add_test(NAME TS-009.mid-burst-recovery
         COMMAND test_decode midburst ${FIXTURES}/e450_serial8.hex 17)
add_test(NAME TS-010.mid-burst-later
         COMMAND test_decode midburst ${FIXTURES}/e450_serial8.hex 200)

# FR-MTR-06 again, from the other side. TS-009/010 open inside a frame and rely
# on the decoder finding the next one. These two start on a flag that opens
# nothing — the `7E 7E` that sits at every frame boundary on the wire.
add_test(NAME TS-023.leading-flags-skipped
         COMMAND test_decode leadflags ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-024.midframe-prefix-then-boundary
         COMMAND test_decode midprefix ${FIXTURES}/e450_serial8.hex)

add_test(NAME TS-011.crc-invalid-frame-discarded
         COMMAND test_decode crcdrop ${FIXTURES}/e450_serial8.hex)
add_test(NAME TS-013.identity-absent
         COMMAND test_decode noidentity ${FIXTURES}/e450_serial8.hex)

add_test(NAME TS-026.one-publish-per-cycle COMMAND test_cores oneset)
add_test(NAME TS-027.first-occurrence-retained COMMAND test_cores firstwins)
add_test(NAME TS-029.empty-set-not-published COMMAND test_cores emptyset)

add_test(NAME TS-102.config-validity COMMAND test_cores config)

add_test(NAME TS-104.ap-identity COMMAND test_cores apident)

add_test(NAME TS-105.indications-distinguishable COMMAND test_cores leds)

add_test(NAME TS-006.cycle-boundary COMMAND test_cores cycle)
add_test(NAME TS-007.obis-mapping   COMMAND test_cores obis)
add_test(NAME TS-008.discovery-payloads COMMAND test_cores discovery)
