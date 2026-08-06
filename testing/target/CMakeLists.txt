# Target-tier tests: one image, every case selected by name over the console.
#
# One image and not one per case, because a build-and-flash cycle costs about
# five minutes on this board and paying it per test would make the tier
# unaffordable — which is how a project ends up with ten target cases and no way
# to run any of them. The cost is paid per change instead.
#
#   idf.py -C testing/target set-target esp32c3
#   idf.py -C testing/target build flash monitor
#
# Then, on the console: `run TS-014`, or `run all`.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../main")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gplug-target-tests)
