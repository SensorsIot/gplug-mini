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
# The firmware's own `main` is deliberately NOT pulled in. These cases ask the
# chip about itself — pad configuration, NVS behaviour, watchdog subscription —
# and none of them needs our decoder. Including it dragged the dlms_parser
# component into this build under different warning flags, where its printf
# format specifiers are -Werror=format errors, and a third-party component's
# warnings have no business failing a target-tier build.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gplug-target-tests)
