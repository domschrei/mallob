
# Version of mallob: By default, current date, user, and machine when building.
MALLOB_VERSION:=$(shell date --iso-8601=seconds)_$(shell whoami)@$(shell hostname)

# Compiler, warning and error settings
CXX=mpicxx
CWARN=-Wno-unused-parameter -Wno-sign-compare -Wno-format -Wno-format-security
CERROR=-fpermissive


# Base compile flags, modified below by individual targets.
COMPILEFLAGS:=-pipe -Wall -Wextra -pedantic -std=c++17 $(CWARN) $(CERROR) -DMALLOB_VERSION=\"${MALLOB_VERSION}\"
# Base linker flags, modified below by individual targets.
LINKERFLAGS:=-L${MPI_ROOT} -Lsrc/app/sat/hordesat/lingeling -Lsrc/app/sat/hordesat/yalsat -Lsrc/app/sat/hordesat/cadical -lm -lz -llgl -lyals -lpthread -lcadical -lrt

# Include paths of the project.
INCLUDES:=-Isrc -I${MPI_INCLUDE}


# All non-test .cpp sources of the project except for main.cpp 
# (so that SOURCES can be used together with other main methods as well).
SOURCES:=$(shell find src/ -name '*.cpp'|grep -vE "src/test/|/main.cpp")

# Test sources: all .cpp files in src/test.
TESTSOURCES:=$(shell find src/test/ -name '*.cpp')


# Target "debug"
debug: COMPILEFLAGS += -O0 -DDEBUG -g -rdynamic
debug: LINKERFLAGS += -O0 -g -rdynamic
debug: build/mallob
debug: build/app/sat/mallob_sat_process

# Target "release"
release: COMPILEFLAGS += -O3 -DNDEBUG
release: LINKERFLAGS += -O3
release: build/mallob
release: build/sat/mallob_sat_process

# Target "tests"
tests: COMPILEFLAGS += -O0 -DDEBUG -g -rdynamic
tests: LINKERFLAGS += -O0 -g -rdynamic
tests: $(patsubst src/test/%.cpp,build/test/%,${TESTSOURCES})

# Target "clean"
clean:
	rm -rf build/


# Build rules

build/mallob: build/main.o $(patsubst src/%.cpp,build/%.o,${SOURCES})
	${CXX} ${INCLUDES} $^ -o build/mallob ${LINKERFLAGS}

build/app/sat/mallob_sat_process: build/app/sat/main.o $(patsubst src/%.cpp,build/%.o,${SOURCES})
	mkdir -p $(@D)
	${CXX} ${INCLUDES} $^ -o build/app/sat/mallob_sat_process ${LINKERFLAGS}

build/test/%: build/test/%.o $(patsubst src/%.cpp,build/%.o,${SOURCES})
	mkdir -p $(@D)
	${CXX} ${INCLUDES} $^ -o $(patsubst %.o,%,$@) ${LINKERFLAGS}

build/%.o: src/%.cpp
	mkdir -p $(@D)
	${CXX} ${COMPILEFLAGS} ${INCLUDES} -o $@ -c $<
