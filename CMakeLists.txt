# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly

# Force Ninja to use response files to completely solve the 32KB Windows command line length limit
if("${CMAKE_GENERATOR}" MATCHES "Ninja")
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE 1 CACHE INTERNAL "")
endif()

cmake_minimum_required(VERSION 3.16)

if(WIN32 AND DEFINED ENV{SENSAIR_LOCAL_TOOLCHAIN})
    file(TO_CMAKE_PATH "$ENV{SENSAIR_LOCAL_TOOLCHAIN}" SENSAIR_LOCAL_TOOLCHAIN_CMAKE)
    set(ENV{PATH} "${SENSAIR_LOCAL_TOOLCHAIN_CMAKE};$ENV{PATH}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensair_pet_brookesia)

# Temporary workaround for `espressif__mcp-c-sdk` stringop-truncation warning
idf_build_set_property(COMPILE_OPTIONS -Wno-error=stringop-truncation APPEND)
