
# Add MaxSAT-specific sources to main Mallob executable
set(MAXSAT_MALLOB_SOURCES src/app/sat/solvers/cadical.cpp src/app/sat/solvers/portfolio_solver_interface.cpp src/app/maxsat/parse/maxsat_reader.cpp src/app/maxsat/parse/static_maxsat_parser_store.cpp src/app/maxsat/maxsat_solver.cpp src/app/maxsat/maxsat_search_procedure.cpp src/app/maxsat/encoding/cardinality_encoding.cpp src/app/maxsat/encoding/openwbo/enc_adder.cpp)
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${MAXSAT_MALLOB_SOURCES} CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include external libraries as necessary
set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/rustsat CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} rustsat dl CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/rustsat/capi CACHE INTERNAL "")
if(MALLOB_USE_MAXPRE)
    add_definitions(-DMALLOB_USE_MAXPRE=1)
    set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/maxpre-mallob CACHE INTERNAL "")
    set(BASE_LIBS ${BASE_LIBS} maxpre CACHE INTERNAL "")
    set(BASE_INCLUDES ${BASE_INCLUDES} lib/maxpre-mallob/src CACHE INTERNAL "")
else()
    add_definitions(-DMALLOB_USE_MAXPRE=0)
endif()

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
new_test(rustsat_encoders "${BASE_INCLUDES}" mallob_core)
new_test(interval_search "${BASE_INCLUDES}" mallob_core)
new_test(cardinality_encoding "${BASE_INCLUDES}" "mallob_corepluscomm;mallob_sat_subproc")
if(MALLOB_USE_MAXPRE)
    new_test(maxpre "${BASE_INCLUDES}" mallob_core)
endif()
# ...
