cmake_minimum_required(VERSION 3.11.4)
project (mallob)

find_package(MPI REQUIRED)

set(CMAKE_CXX_STANDARD 17)


# Build-specific compile options

if(CMAKE_BUILD_TYPE MATCHES DEBUG)
    add_definitions(-DMALLOB_VERSION=\"dbg\" -DMALLOB_ASSERT=1)
    set(MY_DEBUG_OPTIONS "-rdynamic -g -ggdb") 
else()
    add_definitions(-DMALLOB_VERSION=\"rls\")
    set(MY_DEBUG_OPTIONS "-rdynamic -g -ggdb")
    if(MALLOB_ASSERT_HEAVY)
        add_definitions(-DMALLOB_ASSERT=2)
    elseif(MALLOB_ASSERT)
        add_definitions(-DMALLOB_ASSERT=1)
    endif() 
endif()

if(MALLOB_LOG_VERBOSITY)
    add_definitions(-DLOGGER_STATIC_VERBOSITY=${MALLOB_LOG_VERBOSITY})
endif()

if(MALLOB_SUBPROC_DISPATCH_PATH)
    add_definitions(-DMALLOB_SUBPROC_DISPATCH_PATH=${MALLOB_SUBPROC_DISPATCH_PATH})
endif()

if(MALLOB_USE_ASAN)
    set(MY_DEBUG_OPTIONS "${MY_DEBUG_OPTIONS} -fno-omit-frame-pointer -fsanitize=address -static-libasan") 
endif()

if(MALLOB_USE_TBBMALLOC)
    set(BASE_LIBS tbbmalloc_proxy ${BASE_LIBS})
endif()

if(MALLOB_USE_JEMALLOC)
    if(MALLOB_JEMALLOC_DIR)
        link_directories(${MALLOB_JEMALLOC_DIR})
    endif()
    set(BASE_LIBS jemalloc ${BASE_LIBS})
endif()

# Libraries and includes

# Add new default solvers here
link_directories(lib/lingeling lib/yalsat lib/cadical lib/kissat)
set(BASE_LIBS ${BASE_LIBS} ${MPI_CXX_LIBRARIES} ${MPI_CXX_LINK_FLAGS} m z lgl yals pthread cadical kissat rt dl)
set(BASE_INCLUDES ${MPI_CXX_INCLUDE_PATH} src lib/lingeling lib)

set(MALLOB_SOLVERS "lingeling yalsat cadical kissat") # add new default solvers here
if(MALLOB_USE_GLUCOSE)
    set(MALLOB_SOLVERS "${MALLOB_SOLVERS} glucose")
    link_directories(lib/glucose)
    set(BASE_SOURCES ${BASE_SOURCES} src/app/sat/solvers/glucose.cpp)
    set(BASE_LIBS ${BASE_LIBS} glucose)
    set(BASE_INCLUDES ${BASE_INCLUDES} lib/glucose)
    add_definitions(-DMALLOB_USE_GLUCOSE)
endif()    
if(MALLOB_USE_MERGESAT)
    link_directories(lib/mergesat)
    set(BASE_SOURCES ${BASE_SOURCES} src/app/sat/solvers/mergesat.cpp)
    set(BASE_LIBS ${BASE_LIBS} mergesat)
    add_definitions(-DMALLOB_USE_MERGESAT)
endif()    
# Add new non-default solvers here


# Source files

set(BASE_SOURCES ${BASE_SOURCES}
    src/app/job.cpp 
    src/app/dummy/dummy_reader.cpp
    src/app/sat/execution/engine.cpp src/app/sat/execution/solver_thread.cpp src/app/sat/execution/solving_state.cpp
    src/app/sat/job/anytime_sat_clause_communicator.cpp src/app/sat/job/forked_sat_job.cpp src/app/sat/job/threaded_sat_job.cpp src/app/sat/job/sat_process_adapter.cpp src/app/sat/job/sat_process_config.cpp 
    src/app/sat/sharing/buffer/adaptive_clause_database.cpp src/app/sat/sharing/buffer/buffer_merger.cpp src/app/sat/sharing/buffer/buffer_reader.cpp
    src/app/sat/sharing/filter/clause_filter.cpp
    src/app/sat/sharing/sharing_manager.cpp
    src/app/sat/solvers/cadical.cpp src/app/sat/solvers/kissat.cpp src/app/sat/solvers/lingeling.cpp src/app/sat/solvers/portfolio_solver_interface.cpp
    src/balancing/collective_assignment.cpp src/balancing/event_driven_balancer.cpp 
    src/comm/message_queue.cpp src/comm/mpi_base.cpp src/comm/mympi.cpp 
    src/data/job_database.cpp src/data/job_description.cpp src/data/job_reader.cpp src/data/job_result.cpp src/data/job_transfer.cpp 
    src/interface/json_interface.cpp src/interface/api/api_connector.cpp
    src/scheduling/job_scheduling_update.cpp
    src/util/logger.cpp src/util/option.cpp src/util/params.cpp src/util/permutation.cpp src/util/random.cpp src/util/sat_reader.cpp 
    src/util/sys/atomics.cpp src/util/sys/fileutils.cpp src/util/sys/process.cpp src/util/sys/proc.cpp src/util/sys/shared_memory.cpp src/util/sys/terminator.cpp src/util/sys/threading.cpp src/util/sys/thread_pool.cpp src/util/sys/timer.cpp src/util/sys/watchdog.cpp
)


# Library with source files common to mallob and mallob_sat_process

add_library(mallob_commons
    STATIC ${BASE_SOURCES}
)
target_include_directories(mallob_commons PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_commons PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_commons ${BASE_LIBS})


# Executables

add_executable(mallob src/client.cpp src/worker.cpp src/main.cpp)
add_executable(mallob_sat_process src/app/sat/main.cpp)
add_executable(mallob_process_dispatcher src/app/sat/process_dispatcher.cpp)

target_include_directories(mallob PRIVATE ${BASE_INCLUDES})
target_include_directories(mallob_sat_process PRIVATE ${BASE_INCLUDES})
target_include_directories(mallob_process_dispatcher PRIVATE ${BASE_INCLUDES})

target_compile_options(mallob PRIVATE ${BASE_COMPILEFLAGS})
target_compile_options(mallob_sat_process PRIVATE ${BASE_COMPILEFLAGS})
target_compile_options(mallob_process_dispatcher PRIVATE ${BASE_COMPILEFLAGS})

target_link_libraries(mallob ${BASE_LIBS} mallob_commons)
target_link_libraries(mallob_sat_process ${BASE_LIBS} mallob_commons)
target_link_libraries(mallob_process_dispatcher ${BASE_LIBS} mallob_commons) 


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
new_test(clause_database)
new_test(import_buffer)
new_test(clause_filter)
new_test(variable_translator)
new_test(job_description)
new_test(message_queue)
new_test(volume_calculator)
new_test(concurrent_malloc)
new_test(distributed_clause_filter)
new_test(hashing)
