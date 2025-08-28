
# Add SATCNC-specific sources to main Mallob executable
set(SATCNC_MALLOB_SOURCES )
set(MALLOB_COREPLUSCOMM_SOURCES ${MALLOB_COREPLUSCOMM_SOURCES} ${SATCNC_MALLOB_SOURCES} CACHE INTERNAL "")

# Include external libraries as necessary
# ...

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...
