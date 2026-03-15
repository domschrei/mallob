
add_custom_target(app_satcnc ALL)
add_dependencies(app_satcnc app_incsat)

# Add SATCNC-specific sources to main Mallob executable
# (we need cadical.cpp and portfolio_....cpp due to the SatJobStream's internal sequential SAT solver)
set(SATCNC_MALLOB_SOURCES src/app/sat/solvers/cadical.cpp src/app/sat/solvers/portfolio_solver_interface.cpp)
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SATCNC_MALLOB_SOURCES} CACHE INTERNAL "")

# Include external libraries as necessary
# ...

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...
