
add_custom_target(app_satwithpre ALL)
add_dependencies(app_satwithpre app_sat)

# Add MaxSAT-specific sources to main Mallob executable
set(SATWITHPRE_MALLOB_SOURCES 
    src/app/sat/solvers/kissat.cpp 
    src/app/sat/solvers/lingeling.cpp 
    src/app/sat/solvers/portfolio_solver_interface.cpp
    )
set(MALLOB_COREPLUSCOMM_SOURCES 
    ${MALLOB_COREPLUSCOMM_SOURCES} 
    ${SATWITHPRE_MALLOB_SOURCES} 
    CACHE INTERNAL "")

#message("commons+SAT sources: ${BASE_SOURCES}") # Use to debug

# Include external libraries as necessary

if(MALLOB_USE_SATSUMA EQUAL 0)
    add_definitions(-DMALLOB_USE_SATSUMA=0)
else()
    add_definitions(-DMALLOB_USE_SATSUMA=1)

    message("* Registering dependency Satsuma")
    add_custom_command(
        OUTPUT ${CMAKE_SOURCE_DIR}/lib/satsuma/libsatsuma.a
        COMMAND bash fetch-and-build.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/satsuma/
        COMMENT "Building Satsuma library..."
    )
    add_custom_target(dep_satsuma DEPENDS ${CMAKE_SOURCE_DIR}/lib/satsuma/libsatsuma.a)
    
    # Add Satsuma to core dependencies so that it's built first
    set(MALLOB_CORE_DEPS ${MALLOB_CORE_DEPS} dep_satsuma CACHE INTERNAL "")
    set(BASE_LINK_DIRS ${BASE_LINK_DIRS} ${CMAKE_SOURCE_DIR}/lib/satsuma CACHE INTERNAL "")
    set(BASE_LIBS ${BASE_LIBS} satsuma CACHE INTERNAL "")    
    set(BASE_INCLUDES ${BASE_INCLUDES} ${CMAKE_SOURCE_DIR}/lib/satsuma/include CACHE INTERNAL "")
endif()

# Add unit tests: for each $arg there must be a standalone cpp file under "test/test_${arg}.cpp".
# ...

