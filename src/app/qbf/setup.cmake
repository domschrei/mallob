
# QBF-specific sources
set(SAT_SOURCES )

# Add SAT-specific sources to main Mallob executable
set(BASE_SOURCES ${BASE_SOURCES} ${SAT_SOURCES} CACHE INTERNAL "")

# Include external libraries, e.g.:
#link_directories(lib/lingeling lib/yalsat lib/kissat)
#set(BASE_LIBS ${BASE_LIBS} lgl yals cadical kissat CACHE INTERNAL "")
#set(BASE_INCLUDES ${BASE_INCLUDES} lib/lingeling CACHE INTERNAL "") # need to include some lingeling code

# Add new non-default solvers here, e.g.:
#if(MALLOB_USE_GLUCOSE)
#    link_directories(lib/glucose)
#    set(BASE_SOURCES ${BASE_SOURCES} src/app/sat/solvers/glucose.cpp CACHE INTERNAL "")
#    set(BASE_LIBS ${BASE_LIBS} glucose CACHE INTERNAL "")
#    set(BASE_INCLUDES ${BASE_INCLUDES} lib/glucose CACHE INTERNAL "")
#    add_definitions(-DMALLOB_USE_GLUCOSE)
#endif()    
#if(MALLOB_USE_MERGESAT)
#    link_directories(lib/mergesat)
#    set(BASE_SOURCES ${BASE_SOURCES} src/app/sat/solvers/mergesat.cpp CACHE INTERNAL "")
#    set(BASE_LIBS ${BASE_LIBS} mergesat CACHE INTERNAL "")
#    add_definitions(-DMALLOB_USE_MERGESAT)
#endif()
# Further solvers here ...

# Executable of subprocess (if necessary)
#add_executable(mallob_sat_process ${SAT_SOURCES} src/app/sat/main.cpp)
#target_include_directories(mallob_sat_process PRIVATE ${BASE_INCLUDES})
#target_compile_options(mallob_sat_process PRIVATE ${BASE_COMPILEFLAGS})
#target_link_libraries(mallob_sat_process mallob_commons)

# Add unit tests
new_test(qbf_reader)
#...

add_executable(qbf_bloqqer
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/qbf/execution/bloqqer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/qbf/execution/blqrcfg.c)
