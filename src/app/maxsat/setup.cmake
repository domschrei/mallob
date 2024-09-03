
# MaxSAT-specific sources for the sub-process (N/A in our case so far)
set(MAXSAT_SUBPROC_SOURCES "" CACHE INTERNAL "")

# Add MaxSAT-specific sources to main Mallob executable
set(MAXSAT_MALLOB_SOURCES "")
set(BASE_SOURCES ${BASE_SOURCES} ${MAXSAT_MALLOB_SOURCES} CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include external libraries if necessary
#link_directories(lib/lingeling lib/yalsat lib/cadical lib/kissat)
#set(BASE_LIBS ${BASE_LIBS} lgl yals cadical kissat CACHE INTERNAL "")
#set(BASE_INCLUDES ${BASE_INCLUDES} lib CACHE INTERNAL "") # need to include some solver code

# Executable of subprocess (N/A in our case so far)
#add_executable(mallob_maxsat_process ${SAT_SUBPROC_SOURCES} src/app/maxsat/main.cpp)
#target_include_directories(mallob_maxsat_process PRIVATE ${BASE_INCLUDES})
#target_compile_options(mallob_maxsat_process PRIVATE ${BASE_COMPILEFLAGS})
#target_link_libraries(mallob_maxsat_process mallob_commons)

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
#new_test(maxsat_reader)
# ...
