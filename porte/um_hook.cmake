# Pokémon Ultra Moon: included by CMake right after Azahar's project() (CMAKE_PROJECT_INCLUDE) in the
# Ultra Moon's own copy of Azahar (PORTE OFICIAL DO SUN & MOON\azahar, build in build_um). Defers adding
# the Ultra Moon target until the whole Azahar tree has been processed. Nothing of the X / Y port.
if (NOT PORTE_UM_HOOK_DONE)
    set(PORTE_UM_HOOK_DONE ON)
    set(PORTE_UM_ROOT "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL include "${PORTE_UM_ROOT}/CMakeLists.txt")
endif()
