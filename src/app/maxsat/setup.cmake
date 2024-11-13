
# MaxSAT-specific sources for the sub-process (N/A in our case so far)
set(MAXSAT_SUBPROC_SOURCES "" CACHE INTERNAL "")

# Add MaxSAT-specific sources to main Mallob executable
set(MAXSAT_MALLOB_SOURCES src/app/maxsat/parse/maxsat_reader.cpp src/app/maxsat/parse/static_maxsat_parser_store.cpp src/app/maxsat/maxsat_solver.cpp src/app/maxsat/maxsat_search_procedure.cpp src/app/maxsat/encoding/cardinality_encoding.cpp src/app/maxsat/encoding/openwbo/enc_adder.cpp)
set(BASE_SOURCES ${BASE_SOURCES} ${MAXSAT_MALLOB_SOURCES} CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include external libraries as necessary
set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/rustsat CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} rustsat CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/rustsat/capi CACHE INTERNAL "")
if(MALLOB_USE_MAXPRE)
    add_definitions(-DMALLOB_USE_MAXPRE=1)
    set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/maxpre-mallob CACHE INTERNAL "")
    set(BASE_LIBS ${BASE_LIBS} maxpre CACHE INTERNAL "")
    set(BASE_INCLUDES ${BASE_INCLUDES} lib/maxpre-mallob/src CACHE INTERNAL "")
else()
    add_definitions(-DMALLOB_USE_MAXPRE=0)
endif()

# Executable of subprocess (N/A in our case so far)
#add_executable(mallob_maxsat_process ${MAXSAT_SUBPROC_SOURCES} src/app/maxsat/main.cpp)
#target_include_directories(mallob_maxsat_process PRIVATE ${BASE_INCLUDES})
#target_compile_options(mallob_maxsat_process PRIVATE ${BASE_COMPILEFLAGS})
#target_link_libraries(mallob_maxsat_process mallob_commons)

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
new_test(rustsat_encoders)
new_test(interval_search)
if(MALLOB_USE_MAXPRE)
    new_test(maxpre)
endif()
# ...
