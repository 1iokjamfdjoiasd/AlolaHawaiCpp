# Pokémon Ultra Moon - teste de recompilação. Included from teste de porte/porte_x/CMakeLists.txt
# (like porte_vk): the same frontend as Pokémon X/Y Vulkan (PORTE_GAME_UM), plus
#  - arm_dynarmic_recomp.cpp: Azahar's JIT glue with CallSVC offering calls to the native code
#    (an object of the exe: libcitra_core.a's own member is then never pulled in);
#  - recomp_host.cpp: loads momiji_nativo.dll, patches the game code, runs the C functions.
# The exe goes straight to ../bin, next to momiji_nativo.dll.
set(PORTE_UM_DIR "${CMAKE_CURRENT_LIST_DIR}")
# (its own build: the X / Y CMake that used to define it is not included any more)
if(NOT PORTE_WINDRES)
    find_program(PORTE_WINDRES windres REQUIRED)
endif()
set(UM_RES "${CMAKE_CURRENT_BINARY_DIR}/PokemonUM-Recomp-res.o")
add_custom_command(OUTPUT "${UM_RES}"
    COMMAND "${PORTE_WINDRES}" -c 65001 -I "${PORTE_UM_DIR}" -O coff -i "${PORTE_UM_DIR}/PokemonUM-Recomp.rc" -o "${UM_RES}"
    DEPENDS "${PORTE_UM_DIR}/PokemonUM-Recomp.rc" "${PORTE_UM_DIR}/PokemonUM.ico" VERBATIM)
set_source_files_properties("${UM_RES}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
# the folder name has an "&", which breaks the cmd.exe line that links the exe: compile copies
# kept in the build folder (configure_file copies again whenever the originals change)
set(UM_COPY "${CMAKE_CURRENT_BINARY_DIR}/porte_um")
# the Ultra Moon has its own copy of the frontend (porte_recomp/frontend): nothing of the X / Y port
# folder is used or changed
foreach(f main.cpp geocap.cpp geocap.h interp_rec.cpp interp_rec.h framegen_vk.cpp framegen_vk.h vk_present_window.cpp porte_bridge.h font_atlas.h porte_log.h discord_rpc.h)
    configure_file("${PORTE_UM_DIR}/frontend/${f}" "${UM_COPY}/frontend/${f}" COPYONLY)
endforeach()
foreach(f arm_dynarmic_recomp.cpp arm_dyncom_recomp.cpp arm_dyncom_interpreter_recomp.cpp ldr_ro_recomp.cpp recomp_host.cpp recomp_host.h jogos.h)
    configure_file("${PORTE_UM_DIR}/${f}" "${UM_COPY}/${f}" COPYONLY)
endforeach()
file(GLOB UM_UC_HEADERS RELATIVE "${PORTE_UM_DIR}" "${PORTE_UM_DIR}/unicorn/*.h")
foreach(f ${UM_UC_HEADERS})
    configure_file("${PORTE_UM_DIR}/${f}" "${UM_COPY}/${f}" COPYONLY)
endforeach()
add_executable(PokemonUltraMoonRecomp
    "${UM_COPY}/frontend/main.cpp"
    "${UM_COPY}/frontend/geocap.cpp"
    "${UM_COPY}/frontend/interp_rec.cpp"
    "${UM_COPY}/frontend/framegen_vk.cpp"
    "${UM_COPY}/frontend/vk_present_window.cpp"
    "${UM_COPY}/arm_dynarmic_recomp.cpp"
    "${UM_COPY}/arm_dyncom_recomp.cpp"
    "${UM_COPY}/arm_dyncom_interpreter_recomp.cpp"
    "${UM_COPY}/recomp_host.cpp"
    "${UM_COPY}/ldr_ro_recomp.cpp"
    "${UM_RES}")
set_target_properties(PokemonUltraMoonRecomp PROPERTIES OUTPUT_NAME "PokemonUltraMoon-Recomp"
    RUNTIME_OUTPUT_DIRECTORY "${PORTE_UM_DIR}/../bin" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PORTE_UM_DIR}/../bin")
target_include_directories(PokemonUltraMoonRecomp PRIVATE "${CMAKE_SOURCE_DIR}/src" "${UM_COPY}/frontend" "${UM_COPY}"
    "${CMAKE_SOURCE_DIR}/externals/dynarmic/src")
target_link_libraries(PokemonUltraMoonRecomp PRIVATE citra_common citra_core video_core audio_core input_common vulkan-headers vma SDL2::SDL2 dynarmic)
target_compile_definitions(PokemonUltraMoonRecomp PRIVATE SDL_MAIN_HANDLED UNICODE _UNICODE PORTE_GAME_UM PORTE_RECOMP_ON)
get_directory_property(UM_SRC_DEFS DIRECTORY "${CMAKE_SOURCE_DIR}/src" COMPILE_DEFINITIONS)
target_compile_definitions(PokemonUltraMoonRecomp PRIVATE ${UM_SRC_DEFS})
target_link_libraries(PokemonUltraMoonRecomp PRIVATE comdlg32 shell32 ole32 crypt32 winmm)
target_link_options(PokemonUltraMoonRecomp PRIVATE -static-libgcc -static-libstdc++ -s -mwindows)
