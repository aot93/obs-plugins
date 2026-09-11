cmake_minimum_required(VERSION 3.16)
project(obs-datetime-source VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- libobs ------------------------------------------------------------
# If OBS Studio was installed system-wide with CMake package config exported
# (e.g. built with `cmake --install`), this picks it up automatically.
find_package(libobs QUIET)

if(NOT TARGET OBS::libobs)
    # Otherwise, point CMake at an existing obs-studio checkout/build, e.g.:
    #   cmake -B build \
    #     -DLIBOBS_INCLUDE_DIR=/path/to/obs-studio/libobs \
    #     -DLIBOBS_LIB=/path/to/obs-studio/build/libobs/libobs.so
    if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIB)
        message(FATAL_ERROR
            "libobs was not found via find_package(). Build or install OBS Studio "
            "(https://github.com/obsproject/obs-studio) then re-run CMake with:\n"
            "  -DLIBOBS_INCLUDE_DIR=/path/to/obs-studio/libobs\n"
            "  -DLIBOBS_LIB=/path/to/obs-studio/build/libobs/libobs.so")
    endif()
    add_library(OBS::libobs SHARED IMPORTED)
    set_target_properties(OBS::libobs PROPERTIES
        IMPORTED_LOCATION "${LIBOBS_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}")
endif()

# --- Freetype2 (cross-platform text rasterization) ----------------------
find_package(Freetype REQUIRED)

# --- Fontconfig (Linux family/style -> font file resolution) ------------
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
endif()

add_library(obs-datetime-source MODULE
    src/plugin-main.cpp
    src/datetime-source.cpp
    src/datetime-source.h
    src/font-resolver.cpp
    src/font-resolver.h
)

target_link_libraries(obs-datetime-source PRIVATE OBS::libobs Freetype::Freetype)

if(UNIX AND NOT APPLE)
    target_include_directories(obs-datetime-source PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
    target_link_libraries(obs-datetime-source PRIVATE ${FONTCONFIG_LIBRARIES})
    target_compile_definitions(obs-datetime-source PRIVATE HAVE_FONTCONFIG)
endif()

set_target_properties(obs-datetime-source PROPERTIES PREFIX "")

# Ship data/ (locale strings) next to the built module, matching the
# <plugin-name>/bin/<arch> + <plugin-name>/data OBS plugin layout.
add_custom_command(TARGET obs-datetime-source POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "$<TARGET_FILE_DIR:obs-datetime-source>/../data/obs-datetime-source"
)
