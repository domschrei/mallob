cmake_minimum_required(VERSION 3.11.4)
project (mallob)

find_package(MPI REQUIRED)

set(CMAKE_CXX_STANDARD 17)


# Build-specific compile options

if(CMAKE_BUILD_TYPE MATCHES DEBUG)
    add_definitions("-DMALLOB_VERSION=\"dbg\"")
    set(MY_DEBUG_OPTIONS "-rdynamic -g -ggdb") 
else()
    add_definitions("-DMALLOB_VERSION=\"rls\"")
    set(MY_DEBUG_OPTIONS "-rdynamic -g -ggdb") 
endif()

if(MALLOB_USE_ASAN)
    set(MY_DEBUG_OPTIONS "${MY_DEBUG_OPTIONS} -fno-omit-frame-pointer -fsanitize=address -static-libasan") 
endif()


# Libraries and includes

link_directories(lib/lingeling lib/yalsat lib/cadical lib/glucose lib/mergesat)
set(BASE_LIBS ${MPI_CXX_LIBRARIES} ${MPI_CXX_LINK_FLAGS} m z lgl yals pthread cadical mergesat rt dl)
set(BASE_INCLUDES ${MPI_CXX_INCLUDE_PATH} src lib/lingeling lib/glucose lib)


# Source files

set(BASE_SOURCES
    src/app/job.cpp 
    src/app/sat/anytime_sat_clause_communicator.cpp src/app/sat/forked_sat_job.cpp src/app/sat/horde_config.cpp src/app/sat/horde_process_adapter.cpp 
    src/app/sat/hordesat/horde.cpp 
    src/app/sat/hordesat/sharing/default_sharing_manager.cpp 
    src/app/sat/hordesat/solvers/cadical.cpp src/app/sat/hordesat/solvers/lingeling.cpp src/app/sat/hordesat/solvers/mergesat.cpp src/app/sat/hordesat/solvers/portfolio_solver_interface.cpp src/app/sat/hordesat/solvers/solver_thread.cpp src/app/sat/hordesat/solvers/solving_state.cpp 
    src/app/sat/hordesat/utilities/buffer_manager.cpp src/app/sat/hordesat/utilities/clause_database.cpp src/app/sat/hordesat/utilities/clause_filter.cpp 
    src/app/sat/threaded_sat_job.cpp 
    src/balancing/balancer.cpp src/balancing/event_driven_balancer.cpp src/balancing/rounding.cpp 
    src/comm/message_handler.cpp src/comm/mpi_monitor.cpp src/comm/mympi.cpp 
    src/data/job_database.cpp src/data/job_description.cpp src/data/job_file_adapter.cpp src/data/job_reader.cpp src/data/job_result.cpp src/data/reduceable.cpp 
    src/util/logger.cpp src/util/params.cpp src/util/permutation.cpp src/util/random.cpp src/util/sat_reader.cpp 
    src/util/sys/fileutils.cpp src/util/sys/process.cpp src/util/sys/proc.cpp src/util/sys/shared_memory.cpp src/util/sys/terminator.cpp src/util/sys/timer.cpp src/util/sys/watchdog.cpp
    src/util/ringbuf/ringbuf.c
)
set(RESTRICTED_SOURCES src/app/sat/hordesat/solvers/glucose.cpp)

if(MALLOB_USE_RESTRICTED)
    set(BASE_SOURCES ${BASE_SOURCES} ${RESTRICTED_SOURCES})
    set(BASE_LDIRS ${BASE_LDIRS} src/app/sat/hordesat/glucose)
    set(BASE_LIBS ${BASE_LIBS} glucose)
    set(BASE_INCLUDES ${BASE_INCLUDES} src/app/sat/hordesat/glucose)
    add_definitions(-DMALLOB_USE_RESTRICTED)
endif()


# Library with source files common to mallob and mallob_sat_process

add_library(mallob_commons
    STATIC ${BASE_SOURCES}
)
target_include_directories(mallob_commons PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_commons PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_commons ${BASE_LIBS})


# Executables: mallob and mallob_sat_process

add_executable(mallob src/client.cpp src/worker.cpp src/main.cpp)
add_executable(mallob_sat_process src/app/sat/main.cpp)

target_include_directories(mallob PRIVATE ${BASE_INCLUDES})
target_include_directories(mallob_sat_process PRIVATE ${BASE_INCLUDES})

target_compile_options(mallob PRIVATE ${BASE_COMPILEFLAGS})
target_compile_options(mallob_sat_process PRIVATE ${BASE_COMPILEFLAGS})

target_link_libraries(mallob ${BASE_LIBS} mallob_commons)
target_link_libraries(mallob_sat_process ${BASE_LIBS} mallob_commons) 


# Debug flags to find line numbers in stack traces etc.

add_definitions("${MY_DEBUG_OPTIONS}")
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MY_DEBUG_OPTIONS}")


# Tests

enable_testing()

function(new_test testname)
    message(Adding test: ${testname})
    add_executable(test_${testname} src/test/test_${testname}.cpp)
    target_include_directories(test_${testname} PRIVATE ${BASE_INCLUDES})
    target_compile_options(test_${testname} PRIVATE ${BASE_COMPILEFLAGS})
    target_link_libraries(test_${testname} ${BASE_LIBS} mallob_commons)
    add_test(NAME test_${testname} COMMAND test_${testname})
endfunction()

new_test(permutation)
new_test(sat_reader)
new_test(ringbuffer)
new_test(lockfree_clause_database)
new_test(variable_translator)
new_test(job_description)
