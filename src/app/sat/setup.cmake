
# SAT-specific sources for the sub-process
set(SAT_SUBPROC_SOURCES src/app/sat/execution/engine.cpp src/app/sat/execution/solver_thread.cpp src/app/sat/execution/solving_state.cpp src/app/sat/job/sat_process_config.cpp src/app/sat/sharing/buffer/buffer_merger.cpp src/app/sat/sharing/buffer/buffer_reader.cpp src/app/sat/sharing/filter/clause_buffer_lbd_scrambler.cpp src/app/sat/sharing/sharing_manager.cpp src/app/sat/solvers/cadical.cpp src/app/sat/solvers/kissat.cpp src/app/sat/solvers/lingeling.cpp src/app/sat/solvers/portfolio_solver_interface.cpp src/app/sat/data/clause_metadata.cpp src/app/sat/proof/lrat_utils.cpp CACHE INTERNAL "")

# Add SAT-specific sources to main Mallob executable
set(SAT_MALLOB_SOURCES src/app/sat/parse/sat_reader.cpp src/app/sat/execution/solving_state.cpp src/app/sat/job/anytime_sat_clause_communicator.cpp src/app/sat/job/forked_sat_job.cpp src/app/sat/job/sat_process_adapter.cpp src/app/sat/job/sat_process_config.cpp src/app/sat/job/historic_clause_storage.cpp src/app/sat/sharing/buffer/buffer_merger.cpp src/app/sat/sharing/buffer/buffer_reader.cpp src/app/sat/sharing/filter/clause_buffer_lbd_scrambler.cpp src/app/sat/data/clause_metadata.cpp src/app/sat/proof/lrat_utils.cpp)
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SAT_MALLOB_SOURCES} CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include default SAT solvers as external libraries (their Mallob-side interfaces are part of SAT_SOURCES)
set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/lingeling lib/yalsat lib/cadical lib/kissat CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} lgl yals cadical kissat CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib CACHE INTERNAL "") # need to include some solver code

# Add new non-default solvers here
if(MALLOB_USE_GLUCOSE)
    set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/glucose CACHE INTERNAL "")
    set(SAT_SUBPROC_SOURCES ${SAT_SUBPROC_SOURCES} src/app/sat/solvers/glucose.cpp CACHE INTERNAL "")
    set(BASE_LIBS ${BASE_LIBS} glucose CACHE INTERNAL "")
    set(BASE_INCLUDES ${BASE_INCLUDES} lib/glucose CACHE INTERNAL "")
    add_definitions(-DMALLOB_USE_GLUCOSE)
endif()    
if(MALLOB_USE_MERGESAT)
    set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/mergesat CACHE INTERNAL "")
    set(SAT_SUBPROC_SOURCES ${SAT_SUBPROC_SOURCES} src/app/sat/solvers/mergesat.cpp CACHE INTERNAL "")
    set(BASE_LIBS ${BASE_LIBS} mergesat CACHE INTERNAL "")
    add_definitions(-DMALLOB_USE_MERGESAT)
endif()
# Further solvers here ...

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

if(MALLOB_BUILD_LRAT_MODULES)
    # Executable of standalone LRAT checker
    add_executable(standalone_lrat_checker src/app/sat/proof/standalone_checker.cpp)
    target_include_directories(standalone_lrat_checker PRIVATE ${BASE_INCLUDES})
    target_compile_options(standalone_lrat_checker PRIVATE ${BASE_COMPILEFLAGS})
    target_link_libraries(standalone_lrat_checker mallob_sat_subproc)
endif()

# Add unit tests
new_test(hashing "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(random "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(sat_reader "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(clause_database "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(import_buffer "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(variable_translator "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(job_description "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(serialized_formula_parser "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(distributed_file_merger "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(lrat_utils "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(priority_clause_buffer "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(clause_store_iteration "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(lrat_checker "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(portfolio_sequence "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(theory_specification "${BASE_INCLUDES}" mallob_sat_subproc)
new_test(model_string_compressor "${BASE_INCLUDES}" mallob_sat_subproc)
