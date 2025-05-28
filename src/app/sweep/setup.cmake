
# Add compilation units of the application (only the reader code)
set(SWEEP_SOURCES src/app/sweep/sweep_reader.cpp)

# Add these sources to Mallob's base sources
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SWEEP_SOURCES} CACHE INTERNAL "")

#message("commons+DUMMY sources: ${BASE_SOURCES}") # Use to debug

# Done!
