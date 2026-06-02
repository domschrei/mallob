
add_custom_target(app_smt ALL)
add_dependencies(app_smt app_incsat)

# Add MaxSAT-specific sources to main Mallob executable
set(SMT_MALLOB_SOURCES "")
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SMT_MALLOB_SOURCES} CACHE INTERNAL "")

# Library dependency Bitwuzla (and a bunch of additional transitive dependencies)
add_lib_dep("bitwuzla" lib/bitwuzla build/src/lib/ bitwuzla "")

# Find location of libmpfr
find_library(MPFR_LIB NAMES libmpfr.a mpfr)
get_filename_component(MPFR_LIB_DIR "${MPFR_LIB}" DIRECTORY)

# Find location of static libgmp
find_library(GMP_LIB NAMES libgmp.a gmp)
get_filename_component(GMP_LIB_DIR "${GMP_LIB}" DIRECTORY)

set(BASE_LINK_DIRS ${BASE_LINK_DIRS} ${MPFR_LIB_DIR} ${GMP_LIB_DIR} lib/bitwuzla/build/src/ CACHE INTERNAL "")
set(BASE_LIBS ${BASE_LIBS} bzlarng bitwuzlabv bitwuzlabb bitwuzlals bzlautil ${MPFR_LIB} ${GMP_LIB} CACHE INTERNAL "")
set(BASE_INCLUDES ${BASE_INCLUDES} lib/bitwuzla/include/ CACHE INTERNAL "") # need to include some solver code
