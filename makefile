
MALLOB_VERSION?=$(shell date --iso-8601=seconds)"@"$(hostname)

CXX=mpic++
CWARN=-Wno-unused-parameter -Wno-sign-compare -Wno-format -Wno-format-security
CERROR=-fpermissive

#-DNDEBUG
COMPILEFLAGS=-O3 -g -pipe -Wall -Wextra -pedantic -std=c++14 $(CWARN) $(CERROR) -DMALLOB_VERSION=\"${MALLOB_VERSION}\"
#COMPILEFLAGS=-O0 -ggdb -pipe -Wall -Wextra -pedantic -std=c++17 $(CWARN) $(CERROR)

LINKERFLAG=-O3 -L${MPI_ROOT} -Lsrc/hordesat/minisat/build/release/lib -Lsrc/hordesat/lingeling -lm -lz -lminisat -llgl -lpthread
#LINKERFLAG=-O0 -ggdb

INCLUDES=-Isrc -Isrc/hordesat -Isrc/hordesat/lingeling -Isrc/hordesat/minisat -I${MPI_INCLUDE}

#.PHONY = parser clean

build/mallob: $(patsubst src/%.cpp,build/%.o,$(wildcard src/*.cpp src/app/*.cpp src/balancing/*.cpp src/data/*.cpp src/util/*.cpp src/hordesat/sharing/*.cpp src/hordesat/solvers/*.cpp src/hordesat/utilities/*.cpp src/hordesat/*.cpp))
	${CXX} ${INCLUDES} $^ -o build/mallob ${LINKERFLAG}

src/hordesat/minisat/build/release/minisat/core/Solver.o:
	cd src/hordesat && bash fetch_and_build_solvers.sh
src/hordesat/lingeling/lingeling:
	cd src/hordesat && bash fetch_and_build_solvers.sh

build/main.o: src/main.cpp
	${CXX} ${COMPILEFLAGS} ${INCLUDES} -o $@ -c $<

build/%.o: src/%.cpp src/%.h
	mkdir -p $(@D)
	${CXX} ${COMPILEFLAGS} ${INCLUDES} -o $@ -c $<

clean:
	rm -rf build/