
add_custom_target(app_smt ALL)
add_dependencies(app_smt app_incsat)

# Add MaxSAT-specific sources to main Mallob executable
set(SMT_MALLOB_SOURCES "")
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SMT_MALLOB_SOURCES} CACHE INTERNAL "")

# Library dependency Bitwuzla (and a bunch of additional transitive dependencies)
add_lib_dep("bitwuzla" lib/bitwuzla build/src/lib/ bitwuzla "")
set(BASE_LINK_DIRS ${BASE_LINK_DIRS} lib/bitwuzla/build/src/ CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} bzlarng bitwuzlabv bitwuzlabb bitwuzlals bzlautil gmp mpfr CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/bitwuzla/include/ CACHE INTERNAL "") # need to include some solver code
