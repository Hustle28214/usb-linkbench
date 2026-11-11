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

option(BUILD_GUI "Build Qt Widgets dashboard (usb-linkbench-gui)" ON)
if(BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        set(LINKBENCH_QT_TARGET Qt6::Widgets)
        message(STATUS "Building GUI with Qt6")
    else()
        find_package(Qt5 QUIET COMPONENTS Widgets)
        if(Qt5_FOUND)
            set(LINKBENCH_QT_TARGET Qt5::Widgets)
            message(STATUS "Building GUI with Qt5 (Qt6 not found)")
        endif()
    endif()

    if(LINKBENCH_QT_TARGET)
        add_executable(usb-linkbench-gui
            src/gui/main.cpp
            src/gui/main_window.cpp
            src/gui/main_window.hpp
            src/gui/measure_worker.cpp
            src/gui/measure_worker.hpp
            src/gui/speed_gauge.cpp
            src/gui/speed_gauge.hpp
        )
        target_include_directories(usb-linkbench-gui PRIVATE src/gui)
        target_link_libraries(usb-linkbench-gui PRIVATE linkbench ${LINKBENCH_QT_TARGET})
        target_compile_options(usb-linkbench-gui PRIVATE -Wall -Wextra -Wpedantic)
    else()
        message(WARNING "BUILD_GUI=ON but neither Qt6 nor Qt5 Widgets was found; skip usb-linkbench-gui. Install qt6-base-dev (preferred) or qtbase5-dev, or pass -DBUILD_GUI=OFF.")
    endif()
endif()
