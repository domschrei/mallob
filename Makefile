
HORDE = 	src/hordesat/incremental-hordesat
HORDESOLVERS =	src/hordesat

CXXFLAGS =	-O3 -Wall -fmessage-length=0 -Isrc/ -I$(HORDE)/ -D __STDC_LIMIT_MACROS -D __STDC_FORMAT_MACROS

OBJS =		$(HORDE)/HordeLib.o $(wildcard $(HORDE)/utilities/*.o) $(wildcard $(HORDE)/sharing/*.o) $(wildcard $(HORDE)/solvers/*.o)
#OBJS =		$(HORDE)/HordeLib.o
SRCS =		$(wildcard src/*.cpp)
LIBS =		-lz -L$(HORDESOLVERS)/minisat/build/release/lib -lminisat -L$(HORDESOLVERS)/lingeling/ -llgl -lpthread

TARGET =	mallob
CXX =		mpic++

$(TARGET):	$(HORDE)/HordeLib.o
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(OBJS) $(LIBS)

all:	$(TARGET)

$(HORDE)/HordeLib.o:
	make -C $(HORDE) libhordesat.a

clean:
	rm -f $(OBJS) $(TARGET)
