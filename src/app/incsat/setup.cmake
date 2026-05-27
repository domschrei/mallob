
add_custom_target(app_incsat ALL)
add_dependencies(app_incsat app_sat)

# Add IncSAT-specific sources to main Mallob executable
set(INCSAT_MALLOB_SOURCES src/app/sat/stream/sat_job_stream_garbage_collector.cpp src/app/sat/solvers/portfolio_solver_interface.cpp)
if(NOT MALLOB_USE_CADICAL EQUAL 0)
    set(INCSAT_MALLOB_SOURCES ${INCSAT_MALLOB_SOURCES} src/app/sat/solvers/cadical.cpp)
endif()
if(NOT MALLOB_USE_MINISAT EQUAL 0)
    set(INCSAT_MALLOB_SOURCES ${INCSAT_MALLOB_SOURCES} src/app/sat/solvers/minisat.cpp)
endif()

set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${INCSAT_MALLOB_SOURCES} CACHE INTERNAL "")

# Include Bitwuzla as external library (Mallob-side interfaces are part of SMT_SOURCES)
#set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/bitwuzla/build/src/ lib/bitwuzla/build/src/lib/ CACHE INTERNAL "")
#set(BASE_LIBS ${BASE_LIBS} bzlarng bitwuzla bitwuzlabv bitwuzlabb bitwuzlals bzlautil gmp CACHE INTERNAL "")
#set(BASE_INCLUDES ${BASE_INCLUDES} lib/bitwuzla/include/ CACHE INTERNAL "") # need to include some solver code

# Include external libraries as necessary
# ...

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...
