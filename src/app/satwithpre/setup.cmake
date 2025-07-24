
# Add MaxSAT-specific sources to main Mallob executable
set(SATWITHPRE_MALLOB_SOURCES src/app/sat/solvers/kissat.cpp src/app/sat/solvers/lingeling.cpp src/app/sat/solvers/portfolio_solver_interface.cpp)
set(MALLOB_MAINAPP_SOURCES ${MALLOB_MAINAPP_SOURCES} ${SATWITHPRE_MALLOB_SOURCES} CACHE INTERNAL "")

# Need to explicitly link used SAT solver libraries
set(BASE_LIBS ${BASE_LIBS} lgl yals cadical kissat CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include external libraries as necessary

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...
