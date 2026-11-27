cmake_minimum_required(VERSION 3.25)
project(sfeerpakket CXX)

# Bouwen tegen een lokale Dusklight-checkout (ook jullie eigen fork):
#   cmake -B build -DDUSK_DIR=~/pad/naar/dusklight
# De checkout heeft de submodule extern/aurora nodig:
#   git submodule update --init extern/aurora
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dusklight" CACHE PATH "Pad naar de Dusklight-broncode")
    if (NOT EXISTS "${DUSK_DIR}/sdk/CMakeLists.txt")
        message(FATAL_ERROR "DUSK_DIR wijst niet naar een Dusklight-checkout: ${DUSK_DIR}")
    endif ()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
endif ()

add_mod(sfeerpakket
    FEATURES game webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)
