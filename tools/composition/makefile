
CFLAGS=-Wall -O3 --std=c++17

all: build/dratify build/renumber-proofs build/compose-proofs

build/dratify: src/dratify.cpp build/parse.o build/util.o
	mkdir -p build
	g++ $(CFLAGS) build/*.o -o build/dratify src/dratify.cpp -l boost_program_options

build/renumber-proofs: src/renumber.cpp build/parse.o build/util.o
	mkdir -p build
	g++ $(CFLAGS) build/*.o -o build/renumber-proofs src/renumber.cpp -l boost_program_options

build/compose-proofs: src/main.cpp src/*.hpp build/util.o build/parse.o build/join.o build/reverse.o build/prune.o
	mkdir -p build
	g++ $(CFLAGS) build/*.o -o build/compose-proofs src/main.cpp -l boost_program_options

build/join.o: src/join.hpp src/join.cpp src/util.hpp src/parse.hpp
	mkdir -p build
	g++ $(CFLAGS) -c src/join.cpp -o build/join.o

build/reverse.o: src/reverse.hpp src/reverse.cpp
	mkdir -p build
	g++ $(CFLAGS) -c src/reverse.cpp -o build/reverse.o

build/prune.o: src/util.hpp src/parse.hpp src/prune.hpp src/prune.cpp
	mkdir -p build
	g++ $(CFLAGS) -c src/prune.cpp -o build/prune.o

build/parse.o: src/parse.hpp src/parse.cpp src/util.hpp
	mkdir -p build
	g++ $(CFLAGS) -c src/parse.cpp -o build/parse.o

build/util.o: src/util.hpp src/util.cpp
	mkdir -p build
	g++ $(CFLAGS) -c src/util.cpp -o build/util.o


clean:
	rm *~ src/*~ build/*

part-clean:
	rm *~ src/*~
