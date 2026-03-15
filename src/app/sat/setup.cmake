
add_custom_target(app_sat ALL)

# SAT-specific sources for the sub-process
set(SAT_SUBPROC_SOURCES src/app/sat/execution/engine.cpp src/app/sat/execution/solver_thread.cpp
    src/app/sat/execution/solving_state.cpp src/app/sat/sharing/buffer/buffer_merger.cpp 
    src/app/sat/sharing/buffer/buffer_reader.cpp src/app/sat/sharing/filter/clause_buffer_lbd_scrambler.cpp 
    src/app/sat/sharing/sharing_manager.cpp src/app/sat/solvers/portfolio_solver_interface.cpp 
    src/app/sat/data/clause_metadata.cpp src/app/sat/proof/lrat_utils.cpp CACHE INTERNAL "")

# Add SAT-specific sources to main Mallob executable
set(SAT_MALLOB_SOURCES src/app/sat/proof/incremental_trusted_parser_store.cpp src/app/sat/data/formula_compressor.cpp
    src/app/sat/parse/sat_reader.cpp src/app/sat/execution/solving_state.cpp
    src/app/sat/job/anytime_sat_clause_communicator.cpp src/app/sat/job/forked_sat_job.cpp
    src/app/sat/job/sat_process_adapter.cpp src/app/sat/job/historic_clause_storage.cpp
    src/app/sat/sharing/buffer/buffer_merger.cpp src/app/sat/sharing/buffer/buffer_reader.cpp
    src/app/sat/sharing/filter/clause_buffer_lbd_scrambler.cpp src/app/sat/data/clause_metadata.cpp
    src/app/sat/proof/lrat_utils.cpp)
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SAT_MALLOB_SOURCES} CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug


# SAT solver backends. All included by default (i.e., unless explicitly disabled).
if(NOT MALLOB_USE_MINISAT EQUAL 0)
    add_lib_dep("minisat" lib/minisat build/ minisat "src/app/sat/solvers/minisat.cpp")
endif()
if(NOT MALLOB_USE_LINGELING EQUAL 0)
    add_lib_dep("lingeling" lib/lingeling ./ lgl "src/app/sat/solvers/lingeling.cpp")
    add_lib_dep("yalsat" lib/yalsat ./ yals "")
    add_dependencies(dep_lingeling dep_yalsat)
endif()
if(NOT MALLOB_USE_CADICAL EQUAL 0)
    add_lib_dep("cadical" lib/cadical build/ cadical "src/app/sat/solvers/cadical.cpp")
endif()
if(NOT MALLOB_USE_KISSAT EQUAL 0)
    add_lib_dep("kissat" lib/kissat build/ kissat "src/app/sat/solvers/kissat.cpp")
endif()


# ImpCheck binaries for real-time proof checking.
if(MALLOB_BUILD_IMPCHECK EQUAL 1)
    add_definitions(-DMALLOB_USE_IMPCHECK=1)

    # Incremental ImpCheck (IImpCheck)
    message("* Registering dependency IImpCheck (incremental ImpCheck)")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/iimpcheck_confirm
        COMMAND bash fetch-and-build.sh ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lib/iimpcheck/
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/iimpcheck/
        COMMENT "Building dependency IImpCheck"
    )
    add_custom_target(dep_iimpcheck DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/iimpcheck_confirm)
    set(MALLOB_CORE_DEPS ${MALLOB_CORE_DEPS} dep_iimpcheck CACHE INTERNAL "")

    # Verified ImpCheck (ImpCake)
    message("* Registering dependency ImpCake (verified ImpCheck)")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/impcake_confirm
        COMMAND bash fetch-and-build.sh ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lib/impcake/
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lib/impcake/
        COMMENT "Building dependency ImpCake"
    )
    add_custom_target(dep_impcake DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/impcake_confirm)
    set(MALLOB_CORE_DEPS ${MALLOB_CORE_DEPS} dep_impcake CACHE INTERNAL "")
endif()


# Library of SAT subprocess
add_library(mallob_sat_subproc STATIC ${SAT_SUBPROC_SOURCES})
target_include_directories(mallob_sat_subproc PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_sat_subproc PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_sat_subproc ${BASE_LIBS} mallob_core)
target_link_directories(mallob_sat_subproc PUBLIC ${BASE_LINK_DIRS})

# Executable of SAT subprocess
add_executable(mallob_sat_process src/app/sat/main.cpp)
target_include_directories(mallob_sat_process PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_sat_process PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_sat_process mallob_sat_subproc)

# Optional executable of standalone LRAT checker (included by default)
if(NOT MALLOB_BUILD_CHECKER EQUAL 0)
    add_executable(standalone_lrat_checker src/app/sat/proof/standalone_checker.cpp)
    target_include_directories(standalone_lrat_checker PRIVATE ${BASE_INCLUDES})
    target_compile_options(standalone_lrat_checker PRIVATE ${BASE_COMPILEFLAGS})
    target_link_libraries(standalone_lrat_checker mallob_corepluscomm)
endif()


# Add unit tests
new_test(hashing "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(random "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(clause_database "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(import_buffer "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(variable_translator "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(serialized_formula_parser "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(lrat_utils "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(priority_clause_buffer "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(clause_store_iteration "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(lrat_checker "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(portfolio_sequence "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(theory_specification "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(model_string_compressor "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(sat_reader "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(job_description "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(distributed_file_merger "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(formula_compressor "${BASE_INCLUDES}" mallob_corepluscomm)
