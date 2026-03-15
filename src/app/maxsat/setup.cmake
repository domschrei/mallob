
add_custom_target(app_maxsat ALL)
add_dependencies(app_maxsat app_incsat)

# Add MaxSAT-specific sources to main Mallob executable
set(MAXSAT_MALLOB_SOURCES src/app/sat/solvers/cadical.cpp src/app/sat/solvers/portfolio_solver_interface.cpp src/app/maxsat/parse/maxsat_reader.cpp src/app/maxsat/parse/static_maxsat_parser_store.cpp src/app/maxsat/maxsat_solver.cpp src/app/maxsat/encoding/cardinality_encoding.cpp src/app/maxsat/encoding/openwbo/enc_adder.cpp)
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${MAXSAT_MALLOB_SOURCES} CACHE INTERNAL "")

add_lib_dep("rustsat" lib/rustsat target/release/ rustsat_capi "")
set(BASE_LIBS ${BASE_LIBS} dl CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/rustsat/capi CACHE INTERNAL "")

if(MALLOB_USE_MAXPRE EQUAL 0)
    add_definitions(-DMALLOB_USE_MAXPRE=0)
else()
    add_definitions(-DMALLOB_USE_MAXPRE=1)
    add_lib_dep("maxpre" lib/maxpre-mallob ./ maxpre "")
    set(BASE_INCLUDES ${BASE_INCLUDES} lib/maxpre-mallob/src CACHE INTERNAL "")
endif()

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
new_test(rustsat_encoders "${BASE_INCLUDES}" mallob_core)
new_test(interval_search "${BASE_INCLUDES}" mallob_core)
new_test(cardinality_encoding "${BASE_INCLUDES}" "mallob_corepluscomm;mallob_sat_subproc")
if(MALLOB_USE_MAXPRE)
    new_test(maxpre "${BASE_INCLUDES}" mallob_core)
endif()
# ...
