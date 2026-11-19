# This is a fix to compile Mallob on Horeka2, specifically the inner nested Palrup Check which fails due to some missing linking reasons. 

# YOU NEED TO ADD this path export line to your build script, i.e. as
# 
#  export CMAKE_TOOLCHAIN_FILE=MY/PATH/TO/mallob-lto.cmake
#
set(CMAKE_AR     "$ENV{EBROOTGCCCORE}/bin/gcc-ar"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "$ENV{EBROOTGCCCORE}/bin/gcc-ranlib" CACHE FILEPATH "" FORCE)


