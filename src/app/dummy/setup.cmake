
# Add compilation units of the application (only the reader code)
set(DUMMY_SOURCES src/app/dummy/dummy_reader.cpp)

# Add these sources to Mallob's base sources
set(BASE_SOURCES ${BASE_SOURCES} ${DUMMY_SOURCES} CACHE INTERNAL "")

#message("commons+DUMMY sources: ${BASE_SOURCES}") # Use to debug

# Done!
