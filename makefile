
# Version of mallob: By default, current date, user, and machine when building.
MALLOB_VERSION:=$(shell date --iso-8601=seconds)_$(shell whoami)@$(shell hostname)

# Compiler, warning and error settings
CXX=mpicxx
CWARN=-Wno-unused-parameter
CERROR=-fpermissive


# Base compile flags, modified below by individual targets.
COMPILEFLAGS:=-pipe -Wall -Wextra -pedantic -std=c++17 $(CWARN) $(CERROR) -DMALLOB_VERSION=\"${MALLOB_VERSION}\"
# Base linker flags, modified below by individual targets.
LINKERFLAGS:=-L${MPI_ROOT} -Lsrc/app/sat/hordesat/lingeling -Lsrc/app/sat/hordesat/yalsat -Lsrc/app/sat/hordesat/cadical -lm -lz -llgl -lyals -lpthread -lcadical -lrt

# Include paths of the project.
INCLUDES:=-Isrc -I${MPI_INCLUDE}

# Additional flag, library and includes when using restricted software
ifdef MALLOB_USE_RESTRICTED
	COMPILEFLAGS+=-DMALLOB_USE_RESTRICTED
	LINKERFLAGS+=-Lsrc/app/sat/hordesat/glucose -lglucose
	INCLUDES+=-Isrc/app/sat/hordesat/glucose
endif


# All .cpp sources of the project except for 
# - test files
# - solver sources that are built separately and just included / linked
# - any "main.cpp" files.
SOURCES:=$(shell find src/ -name '*.cpp'|grep -vE "src/test/|src/app/sat/hordesat/(cadical|lingeling|yalsat|glucose)|/main.cpp")
ifndef MALLOB_USE_RESTRICTED
	# Explicitly exclude glucose 
	SOURCES:=$(shell find src/ -name '*.cpp'|grep -vE "glucose.cpp|src/test/|src/app/sat/hordesat/(cadical|lingeling|yalsat|glucose)|/main.cpp")
endif

# Test sources: all .cpp files in src/test.
TESTSOURCES:=$(shell find src/test/ -name '*.cpp')


# Target "debug"
debug: COMPILEFLAGS += -O0 -DDEBUG -g -rdynamic -ggdb
debug: LINKERFLAGS += -O0 -g -rdynamic -ggdb
debug: build/mallob
debug: build/app/sat/mallob_sat_process

# Target "release"
release: COMPILEFLAGS += -O3 -DNDEBUG
release: LINKERFLAGS += -O3
release: build/mallob
release: build/app/sat/mallob_sat_process

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
