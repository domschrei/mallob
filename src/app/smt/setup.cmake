
add_custom_target(app_smt ALL)
add_dependencies(app_smt app_incsat)

# Add MaxSAT-specific sources to main Mallob executable
set(SMT_MALLOB_SOURCES src/app/sat/solvers/cadical.cpp src/app/sat/solvers/portfolio_solver_interface.cpp)
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SMT_MALLOB_SOURCES} CACHE INTERNAL "")

add_lib_dep("bitwuzla" lib/bitwuzla build/src/lib/ bitwuzla "")
set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/bitwuzla/build/src/ CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} bzlarng bitwuzlabv bitwuzlabb bitwuzlals bzlautil gmp mpfr CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/bitwuzla/include/ CACHE INTERNAL "") # need to include some solver code

# Include Bitwuzla as external library (Mallob-side interfaces are part of SMT_SOURCES)
#set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/bitwuzla/build/src/ lib/bitwuzla/build/src/lib/ CACHE INTERNAL "")
#set(BASE_LIBS ${BASE_LIBS} bzlarng bitwuzla bitwuzlabv bitwuzlabb bitwuzlals bzlautil gmp mpfr CACHE INTERNAL "")
#set(BASE_INCLUDES ${BASE_INCLUDES} lib/bitwuzla/include/ CACHE INTERNAL "") # need to include some solver code

# Include external libraries as necessary
# ...

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...
