
# Add compilation units of the application
set(KMEANS_SOURCES src/app/kmeans/kmeans_job.cpp src/app/kmeans/kmeans_reader.cpp src/app/kmeans/kmeans_utils.cpp)

# Add these sources to Mallob's base sources
set(BASE_SOURCES ${BASE_SOURCES} ${KMEANS_SOURCES} CACHE INTERNAL "")

#message("commons+DUMMY sources: ${BASE_SOURCES}") # Use to debug

# Done!
