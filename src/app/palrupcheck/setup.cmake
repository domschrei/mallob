
message("* Registering dependency PalRUP checking toolchain")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/lib/palrup/build/plrat_last_pass
    COMMAND bash fetch-and-build.sh ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lib/palrup/
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/palrup/
    COMMENT "Building dependency PalRUP checking toolchain"
)
add_custom_target(dep_palrup DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lib/palrup/build/plrat_last_pass)
set(MALLOB_CORE_DEPS ${MALLOB_CORE_DEPS} dep_palrup CACHE INTERNAL "")
