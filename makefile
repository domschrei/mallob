
MALLOB_VERSION?=$(shell date --iso-8601=seconds)_$(shell whoami)@$(shell hostname)

CXX=mpicxx
CWARN=-Wno-unused-parameter -Wno-sign-compare -Wno-format -Wno-format-security
CERROR=-fpermissive

#-DNDEBUG
COMPILEFLAGS=-O3 -g -pipe -Wall -Wextra -pedantic -std=c++17 $(CWARN) $(CERROR) -DMALLOB_VERSION=\"${MALLOB_VERSION}\"
#COMPILEFLAGS=-O0 -ggdb -pipe -Wall -Wextra -pedantic -std=c++17 $(CWARN) $(CERROR)

LINKERFLAG=-O3 -L${MPI_ROOT} -Lsrc/app/sat/hordesat/lingeling -Lsrc/app/sat/hordesat/yalsat -lm -lz -llgl -lyals -lpthread
#LINKERFLAG=-O0 -ggdb

INCLUDES=-Isrc -I${MPI_INCLUDE}

#.PHONY = parser clean

SOURCES:=$(shell find src/ -name '*.cpp'|grep -v "/test/")

build/mallob: $(patsubst src/%.cpp,build/%.o,${SOURCES})
	${CXX} ${INCLUDES} $^ -o build/mallob ${LINKERFLAG}

src/hordesat/yalsat/yalsat:
	cd src/app/sat/hordesat && bash fetch_and_build_solvers.sh

build/main.o: src/main.cpp
	${CXX} ${COMPILEFLAGS} ${INCLUDES} -o $@ -c $<

build/%.o: src/%.cpp src/%.hpp
	mkdir -p $(@D)
	${CXX} ${COMPILEFLAGS} ${INCLUDES} -o $@ -c $<

clean:
	rm -rf build/