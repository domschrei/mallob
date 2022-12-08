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

# Default application
if(NOT DEFINED MALLOB_APP_SAT)
    set(MALLOB_APP_SAT 1)
endif()


# Libraries and includes

set(BASE_LIBS ${BASE_LIBS} ${MPI_CXX_LIBRARIES} ${MPI_CXX_LINK_FLAGS} m z rt dl CACHE INTERNAL "")
set(BASE_INCLUDES ${MPI_CXX_INCLUDE_PATH} src lib CACHE INTERNAL "")


# Base source files

set(BASE_SOURCES ${BASE_SOURCES} src/app/job.cpp src/app/app_registry.cpp src/balancing/event_driven_balancer.cpp src/balancing/request_matcher.cpp src/balancing/routing_tree_request_matcher.cpp src/comm/msg_queue/message_queue.cpp src/comm/mpi_base.cpp src/comm/mympi.cpp src/comm/sysstate_unresponsive_crash.cpp src/core/scheduling_manager.cpp src/data/job_description.cpp src/data/job_result.cpp src/data/job_transfer.cpp src/interface/json_interface.cpp src/interface/api/api_connector.cpp src/scheduling/job_scheduling_update.cpp src/util/logger.cpp src/util/option.cpp src/util/params.cpp src/util/permutation.cpp src/util/random.cpp src/util/sys/atomics.cpp src/util/sys/fileutils.cpp src/util/sys/process.cpp src/util/sys/proc.cpp src/util/sys/process_dispatcher.cpp src/util/sys/shared_memory.cpp src/util/sys/terminator.cpp src/util/sys/threading.cpp src/util/sys/thread_pool.cpp src/util/sys/timer.cpp src/util/sys/watchdog.cpp CACHE INTERNAL "")

# Use to debug
#message("mallob_commons sources pre application registration: ${BASE_SOURCES}")


# Define test function

function(new_test testname)
    message("Adding test: ${testname}")
    add_executable(test_${testname} src/test/test_${testname}.cpp)
    target_include_directories(test_${testname} PRIVATE ${BASE_INCLUDES})
    target_compile_options(test_${testname} PRIVATE ${BASE_COMPILEFLAGS})
    target_link_libraries(test_${testname} ${BASE_LIBS} mallob_commons)
    add_test(NAME test_${testname} COMMAND test_${testname})
endfunction()


# Add application-specific build configuration

# Setup (do not change)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_includes.h~" "")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_commands.h~" "")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_options.h~" "")
function(register_mallob_app appkey)
    message("Registering application: ${appkey}")
    file(APPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_includes.h~" "#include \"app/${appkey}/register.hpp\"\n")
    file(APPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_options.h~" "#include \"app/${appkey}/options.hpp\"\n")
    file(APPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_commands.h~" "register_mallob_app_${appkey}();\n")
    include(${CMAKE_CURRENT_SOURCE_DIR}/src/app/${appkey}/setup.cmake)
endfunction()
function(publish_app_registry_changes)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files 
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_options.h" 
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_options.h~" 
        RESULT_VARIABLE compare_result)
    if(compare_result EQUAL 0)
        message("No changes in register options.")
    else()
        message("Moving changed app registry.")
        file(RENAME "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_includes.h~" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_includes.h")
        file(RENAME "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_options.h~" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_options.h")
        file(RENAME "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_commands.h~" "${CMAKE_CURRENT_SOURCE_DIR}/src/app/register_commands.h")
    endif() 
endfunction()

# Include applications
register_mallob_app("dummy") # always register the "no-op" application
if(MALLOB_APP_SAT) 
    register_mallob_app("sat")
endif()
if(MALLOB_APP_KMEANS)
    register_mallob_app("kmeans")
endif()
# Include further applications here:
#if(MALLOB_APP_YOURAPP)
#    register_mallob_app("yourapp")
#endif()
# ...
publish_app_registry_changes()


# Library with Mallob's common source files

#message("mallob_commons sources post application registration: ${BASE_SOURCES}") # Use to debug
add_library(mallob_commons
    STATIC ${BASE_SOURCES}
)
target_include_directories(mallob_commons PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_commons PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_commons ${BASE_LIBS})


# Executables

add_executable(mallob src/core/client.cpp src/core/worker.cpp src/main.cpp)
target_include_directories(mallob PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob ${BASE_LIBS} mallob_commons)

add_executable(mallob_process_dispatcher src/app/sat/main_dispatch.cpp)
target_include_directories(mallob_process_dispatcher PRIVATE ${BASE_INCLUDES})
target_compile_options(mallob_process_dispatcher PRIVATE ${BASE_COMPILEFLAGS})
target_link_libraries(mallob_process_dispatcher ${BASE_LIBS} mallob_commons) 


# Debug flags to find line numbers in stack traces etc.

add_definitions("${MY_DEBUG_OPTIONS}")
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MY_DEBUG_OPTIONS}")


# Tests

enable_testing()

new_test(permutation)
new_test(message_queue)
new_test(volume_calculator)
new_test(concurrent_malloc)
new_test(hashing)
new_test(async_collective)
