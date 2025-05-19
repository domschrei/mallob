cmake_minimum_required(VERSION 3.11.4)
project (mallob)

find_package(MPI REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)





#Testing if march=native helps, but at least in quick laptop test, no, no change
#add_compile_options(-march=native)

# Build-specific compile options

set(BASE_LIBS Threads::Threads)
set(MY_DEBUG_OPTIONS "-rdynamic -g -ggdb -Wall -Wno-sign-compare -Wno-unused-variable")

if(CMAKE_BUILD_TYPE MATCHES DEBUG)
    add_definitions(-DMALLOB_VERSION=\"dbg\")
    if(MALLOB_ASSERT_HEAVY)
        add_definitions(-DMALLOB_ASSERT=2)
    else()
        add_definitions(-DMALLOB_ASSERT=1)
    endif()
else()
    add_definitions(-DMALLOB_VERSION=\"rls\")
    if(MALLOB_ASSERT_HEAVY)
        add_definitions(-DMALLOB_ASSERT=2)
    elseif(MALLOB_ASSERT)
        add_definitions(-DMALLOB_ASSERT=1)
    endif() 
endif()

if(MALLOB_LOG_VERBOSITY)
    add_definitions(-DLOGGER_STATIC_VERBOSITY=${MALLOB_LOG_VERBOSITY})
endif()

if(MALLOB_MAX_N_APPTHREADS_PER_PROCESS)
    add_definitions(-DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=${MALLOB_MAX_N_APPTHREADS_PER_PROCESS})
endif()

if(MALLOB_SUBPROC_DISPATCH_PATH)
    add_definitions(-DMALLOB_SUBPROC_DISPATCH_PATH=${MALLOB_SUBPROC_DISPATCH_PATH})
endif()

if(DEFINED MALLOB_TRUSTED_SUBPROCESSING)
    add_definitions(-DMALLOB_TRUSTED_SUBPROCESSING=${MALLOB_TRUSTED_SUBPROCESSING})
endif()

if(MALLOB_USE_ASAN)
    set(MY_DEBUG_OPTIONS "${MY_DEBUG_OPTIONS} -fno-omit-frame-pointer -fsanitize=address,leak,undefined -static-libasan") 
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

# Default application
if(NOT DEFINED MALLOB_APP_SAT)
    set(MALLOB_APP_SAT 1)
endif()


# Libraries and includes

set(BASE_LIBS ${BASE_LIBS} m z rt dl CACHE INTERNAL "")
set(BASE_INCLUDES src src/util/tsl lib
    # We technically don't need to provide the MPI includes here,
    # but it helps generate the correct compile commands for vscodium etc.
    ${MPI_C_INCLUDE_PATH} ${MPI_C_INCLUDE_DIRS} ${MPI_CXX_INCLUDE_PATH} ${MPI_CXX_INCLUDE_DIRS}
    CACHE INTERNAL "")
set(BASE_COMPILEFLAGS CACHE INTERNAL "")


# Base source files

set(MALLOB_CORE_SOURCES
    src/data/app_configuration.cpp src/data/job_description.cpp src/data/job_result.cpp src/interface/json_interface.cpp
    src/interface/api/api_connector.cpp src/interface/api/api_registry.cpp src/scheduling/core_allocator.cpp
    src/util/logger.cpp src/util/option.cpp src/util/params.cpp src/util/permutation.cpp 
    src/util/random.cpp src/util/sys/atomics.cpp src/util/sys/fileutils.cpp src/util/sys/process.cpp src/util/sys/proc.cpp 
    src/util/sys/process_dispatcher.cpp src/util/sys/shared_memory.cpp src/util/sys/tmpdir.cpp src/util/sys/terminator.cpp 
    src/util/sys/threading.cpp src/util/sys/thread_pool.cpp src/util/sys/timer.cpp src/util/sys/watchdog.cpp
    src/util/sys/shmem_cache.cpp src/util/ringbuf/ringbuf.c src/util/static_store.cpp
    CACHE INTERNAL "")

set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_CORE_SOURCES}
    src/app/job.cpp src/app/app_registry.cpp src/app/app_message_subscription.cpp src/balancing/event_driven_balancer.cpp 
    src/balancing/request_matcher.cpp src/balancing/routing_tree_request_matcher.cpp src/comm/msg_queue/message_queue.cpp 
    src/comm/mpi_base.cpp src/comm/mympi.cpp src/comm/sysstate_unresponsive_crash.cpp src/core/scheduling_manager.cpp 
    src/data/job_transfer.cpp src/scheduling/job_scheduling_update.cpp 
    CACHE INTERNAL "")

# Use to debug
#message("mallob CORE sources pre application registration: ${MALLOB_CORE_SOURCES}")
#message("mallob CORE+COMM sources pre application registration: ${MALLOB_COREPLUSCOMM_SOURCES}")


# Define test function

function(new_test testname includes linklibs)
    message("Adding test: ${testname}")
    add_executable(test_${testname} src/test/test_${testname}.cpp)
    target_include_directories(test_${testname} PRIVATE ${includes})
    target_compile_options(test_${testname} PRIVATE ${BASE_COMPILEFLAGS})
    target_link_libraries(test_${testname} ${linklibs})
    add_test(NAME test_${testname} COMMAND test_${testname})
endfunction()


# Add application-specific build configuration

# Setup (do not change)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_includes.h~" "")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_options.h~" "")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_commands.h~" "")
function(register_mallob_app appkey)
    message("Registering application: ${appkey}")
    file(APPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_includes.h~" "#include \"app/${appkey}/register.hpp\"\n")
    file(APPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_options.h~" "#include \"app/${appkey}/options.hpp\"\n")
    file(APPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_commands.h~" "register_mallob_app_${appkey}();\n")
    include(${CMAKE_CURRENT_SOURCE_DIR}/src/app/${appkey}/setup.cmake)
endfunction()
function(publish_app_registry_changes)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files 
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_options.h" 
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_options.h~" 
        RESULT_VARIABLE compare_result)
    if(compare_result EQUAL 0)
        message("No changes in register options.")
    else()
        message("Moving changed app registry.")
        file(RENAME "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_includes.h~" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_includes.h")
        file(RENAME "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_options.h~" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_options.h")
        file(RENAME "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_commands.h~" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/.register_commands.h")
    endif() 
endfunction()

# Include applications
register_mallob_app("dummy") # always register the "no-op" application
if(MALLOB_APP_MAXSAT)
    register_mallob_app("maxsat")
endif()
if(MALLOB_APP_SAT) 
    register_mallob_app("sat")
endif()
if(MALLOB_APP_KMEANS)
    register_mallob_app("kmeans")
endif()
if(MALLOB_APP_SATWITHPRE)
    register_mallob_app("satwithpre")
endif()
# Include further applications here:
#if(MALLOB_APP_YOURAPP)
#    register_mallob_app("yourapp")
#endif()
# ...
publish_app_registry_changes()


# Library of Mallob's CORE sources

#message("mallob_commons sources post application registration: ${BASE_SOURCES}") # Use to debug
add_library(mallob_core STATIC ${MALLOB_CORE_SOURCES})
target_include_directories(mallob_core PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_core PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_core ${BASE_LIBS})
target_link_directories(mallob_core PUBLIC ${BASE_LINK_DIRS})


# Library of Mallob's CORE+COMM sources

#message("mallob_commons sources post application registration: ${BASE_SOURCES}") # Use to debug
add_library(mallob_corepluscomm STATIC ${MALLOB_COREPLUSCOMM_SOURCES})
target_include_directories(mallob_corepluscomm PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_corepluscomm PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_corepluscomm ${MPI_CXX_LIBRARIES} ${MPI_CXX_LINK_FLAGS} mallob_core ${BASE_LIBS})
target_link_directories(mallob_corepluscomm PUBLIC ${BASE_LINK_DIRS})


# Executables

# Mallob main executable
add_executable(mallob src/core/client.cpp src/core/worker.cpp src/main.cpp)
target_include_directories(mallob PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob mallob_corepluscomm)

# Mallob process dispatcher
add_executable(mallob_process_dispatcher src/app/sat/main_dispatch.cpp)
target_include_directories(mallob_process_dispatcher PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_process_dispatcher PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_process_dispatcher mallob_core)


# Debug flags to find line numbers in stack traces etc.

add_definitions("${MY_DEBUG_OPTIONS}")
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MY_DEBUG_OPTIONS}")


# Tests

new_test(bitsets "${BASE_INCLUDES}" mallob_core)
new_test(permutation "${BASE_INCLUDES}" mallob_core)
new_test(message_queue "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(volume_calculator "${BASE_INCLUDES}" mallob_core)
new_test(concurrent_malloc "${BASE_INCLUDES}" mallob_core)
new_test(async_collective "${BASE_INCLUDES}" mallob_corepluscomm)
new_test(reverse_file_reader "${BASE_INCLUDES}" mallob_core)
new_test(categorized_external_memory "${BASE_INCLUDES}" mallob_core)
new_test(bidirectional_pipe "${BASE_INCLUDES}" mallob_core)
new_test(bidirectional_pipe_shmem "${BASE_INCLUDES}" mallob_core)
