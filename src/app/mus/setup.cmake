
# Add compilation units of the application
set(MUS_SOURCES src/app/mus/mus_solver.cpp)

# Add these sources to Mallob's base sources
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${MUS_SOURCES} CACHE INTERNAL "")

# Done!
