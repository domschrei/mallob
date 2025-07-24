
# Add MaxSAT-specific sources to main Mallob executable
set(SMT_MALLOB_SOURCES src/app/sat/solvers/cadical.cpp src/app/sat/solvers/portfolio_solver_interface.cpp)
set(MALLOB_MAINAPP_SOURCES ${MALLOB_MAINAPP_SOURCES} ${SMT_MALLOB_SOURCES} CACHE INTERNAL "")

# Include Bitwuzla as external library (Mallob-side interfaces are part of SMT_SOURCES)
set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/bitwuzla/build/src/ lib/bitwuzla/build/src/lib/ CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} cadical bzlarng bitwuzla bitwuzlabv bitwuzlabb bitwuzlals bzlautil gmp CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/bitwuzla/include/ CACHE INTERNAL "") # need to include some solver code

# Include external libraries as necessary
# ...

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...
