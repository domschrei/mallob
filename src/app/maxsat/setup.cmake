
# MaxSAT-specific sources for the sub-process (N/A in our case so far)
set(MAXSAT_SUBPROC_SOURCES "" CACHE INTERNAL "")

# Add MaxSAT-specific sources to main Mallob executable
set(MAXSAT_MALLOB_SOURCES src/app/maxsat/parse/maxsat_reader.cpp src/app/maxsat/maxsat_solver.cpp)
set(BASE_SOURCES ${BASE_SOURCES} ${MAXSAT_MALLOB_SOURCES} CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include external libraries if necessary
link_directories(lib/rustsat)
set(BASE_LIBS ${BASE_LIBS} rustsat CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/rustsat/capi CACHE INTERNAL "")

# Executable of subprocess (N/A in our case so far)
#add_executable(mallob_maxsat_process ${MAXSAT_SUBPROC_SOURCES} src/app/maxsat/main.cpp)
#target_include_directories(mallob_maxsat_process PRIVATE ${BASE_INCLUDES})
#target_compile_options(mallob_maxsat_process PRIVATE ${BASE_COMPILEFLAGS})
#target_link_libraries(mallob_maxsat_process mallob_commons)

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
#new_test(maxsat_reader)
# ...