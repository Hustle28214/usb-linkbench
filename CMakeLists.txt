cmake_minimum_required(VERSION 3.16)
project(usb-linkbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LINKBENCH_LIBUSB "")
find_package(libusb CONFIG QUIET)
if(TARGET libusb::libusb)
    set(LINKBENCH_LIBUSB libusb::libusb)
elseif(TARGET unofficial::libusb::libusb)
    set(LINKBENCH_LIBUSB unofficial::libusb::libusb)
endif()

if(NOT LINKBENCH_LIBUSB AND LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")
    set(LIBUSB_LIBRARIES "${LIBUSB_LIBRARY}")
    set(LIBUSB_FOUND TRUE)
endif()

if(NOT LINKBENCH_LIBUSB AND NOT LIBUSB_FOUND AND NOT CMAKE_CROSSCOMPILING)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB QUIET libusb-1.0)
    endif()
endif()

if(NOT LINKBENCH_LIBUSB AND (NOT LIBUSB_FOUND OR NOT LIBUSB_LIBRARIES))
    find_path(LIBUSB_INCLUDE_DIR libusb.h
        HINTS ${LIBUSB_ROOT} ENV LIBUSB_ROOT
        PATH_SUFFIXES include include/libusb-1.0)
    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0 libusb-1.0
        HINTS ${LIBUSB_ROOT} ENV LIBUSB_ROOT
        PATH_SUFFIXES lib lib64 MinGW64/dll MinGW64/static VS2022/MS64/dll VS2019/MS64/dll)
    if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
        set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")
        set(LIBUSB_LIBRARIES "${LIBUSB_LIBRARY}")
        set(LIBUSB_FOUND TRUE)
    endif()
endif()

if(NOT LINKBENCH_LIBUSB AND NOT LIBUSB_LIBRARIES)
    message(FATAL_ERROR "libusb-1.0 was not found. On Debian/Ubuntu install libusb-1.0-dev. On macOS: brew install libusb pkg-config. On Windows set LIBUSB_ROOT or use vcpkg.")
endif()

set(LINKBENCH_SOURCES
    src/core.cpp
    src/os.cpp
    src/usb.cpp
    src/stream.cpp
    src/storage.cpp
    src/links.cpp
    src/report.cpp
    src/text.cpp
    src/run.cpp
)
if(WIN32)
    list(APPEND LINKBENCH_SOURCES src/usb_win.cpp src/stream_win.cpp)
endif()
if(APPLE)
    list(APPEND LINKBENCH_SOURCES src/stream_apple.mm)
endif()

add_library(linkbench STATIC ${LINKBENCH_SOURCES})
target_include_directories(linkbench PUBLIC include)
if(LINKBENCH_LIBUSB)
    target_link_libraries(linkbench PUBLIC ${LINKBENCH_LIBUSB})
else()
    target_include_directories(linkbench PUBLIC ${LIBUSB_INCLUDE_DIRS})
    if(LIBUSB_LIBRARY_DIRS)
        target_link_directories(linkbench PUBLIC ${LIBUSB_LIBRARY_DIRS})
    endif()
    target_link_libraries(linkbench PUBLIC ${LIBUSB_LIBRARIES})
endif()

if(MINGW)
    target_link_libraries(linkbench PUBLIC "-Wl,-Bstatic" "-lwinpthread" "-Wl,-Bdynamic")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(linkbench PUBLIC Threads::Threads)
endif()
target_compile_definitions(linkbench PUBLIC LINKBENCH_CATALOG="${CMAKE_SOURCE_DIR}/docs/benchmark-report.md")
if(WIN32)
    target_compile_definitions(linkbench PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0602 _WIN32_WINNT=0x0602 NTDDI_VERSION=0x06020000)
    target_link_libraries(linkbench PUBLIC setupapi ole32 advapi32 cfgmgr32 mfplat mfreadwrite mfuuid mf)
endif()
if(APPLE)
    target_link_libraries(linkbench PUBLIC
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework IOKit")
    set_source_files_properties(src/stream_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

function(linkbench_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(linkbench_catalog target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/docs/benchmark-report.md"
            "$<TARGET_FILE_DIR:${target}>/benchmark-report.md"
        VERBATIM)
endfunction()

linkbench_warnings(linkbench)

add_executable(usb-linkbench src/main.cpp)
target_link_libraries(usb-linkbench PRIVATE linkbench)
linkbench_warnings(usb-linkbench)
linkbench_catalog(usb-linkbench)
if(MINGW)
    target_link_options(usb-linkbench PRIVATE -static-libgcc -static-libstdc++)
endif()

set(LIBUSB_DLL "" CACHE FILEPATH "libusb shared library copied next to usb-linkbench")
if(LIBUSB_DLL)
    add_custom_command(TARGET usb-linkbench POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBUSB_DLL}" "$<TARGET_FILE_DIR:usb-linkbench>"
        VERBATIM)
endif()

add_executable(linkbench-tests tests/unit.cpp)
target_link_libraries(linkbench-tests PRIVATE linkbench)
linkbench_warnings(linkbench-tests)
linkbench_catalog(linkbench-tests)
if(MINGW)
    target_link_options(linkbench-tests PRIVATE -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_test(NAME unit COMMAND linkbench-tests)

if(CMAKE_CROSSCOMPILING)
    set(LINKBENCH_GUI_DEFAULT OFF)
else()
    set(LINKBENCH_GUI_DEFAULT ON)
endif()
option(BUILD_GUI "Build Qt Widgets dashboard (usb-linkbench-gui)" ${LINKBENCH_GUI_DEFAULT})
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
            src/gui/sparkline.cpp
            src/gui/sparkline.hpp
        )
        target_include_directories(usb-linkbench-gui PRIVATE src/gui)
        target_link_libraries(usb-linkbench-gui PRIVATE linkbench ${LINKBENCH_QT_TARGET})
        linkbench_warnings(usb-linkbench-gui)
        linkbench_catalog(usb-linkbench-gui)
        if(MINGW)
            target_link_options(usb-linkbench-gui PRIVATE -static-libgcc -static-libstdc++)
        endif()
        if(LIBUSB_DLL)
            add_custom_command(TARGET usb-linkbench-gui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBUSB_DLL}" "$<TARGET_FILE_DIR:usb-linkbench-gui>"
                VERBATIM)
        endif()
    else()
        message(WARNING "BUILD_GUI=ON but neither Qt6 nor Qt5 Widgets was found; skip usb-linkbench-gui. Install qt6-base-dev (preferred) or qtbase5-dev, or pass -DBUILD_GUI=OFF.")
    endif()
endif()

install(TARGETS usb-linkbench RUNTIME DESTINATION .)
install(FILES docs/benchmark-report.md DESTINATION .)
if(WIN32)
    install(FILES contrib/windows/README.txt DESTINATION .)
endif()
install(FILES LICENSE-MIT LICENSE-APACHE DESTINATION .)
if(LIBUSB_DLL)
    install(FILES "${LIBUSB_DLL}" DESTINATION .)
endif()
