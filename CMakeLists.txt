cmake_minimum_required(VERSION 3.16)
project(usb-linkbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_library(linkbench STATIC
    src/core.cpp
    src/usb.cpp
    src/stream.cpp
    src/storage.cpp
    src/links.cpp
    src/report.cpp
    src/text.cpp
    src/run.cpp
)
target_include_directories(linkbench PUBLIC include ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(linkbench PUBLIC ${LIBUSB_LIBRARIES} pthread)
target_compile_definitions(linkbench PUBLIC LINKBENCH_CATALOG="${CMAKE_SOURCE_DIR}/docs/benchmark-report.md")
target_compile_options(linkbench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(usb-linkbench src/main.cpp)
target_link_libraries(usb-linkbench PRIVATE linkbench)
target_compile_options(usb-linkbench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(linkbench-tests tests/unit.cpp)
target_link_libraries(linkbench-tests PRIVATE linkbench)
target_compile_options(linkbench-tests PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME unit COMMAND linkbench-tests)
