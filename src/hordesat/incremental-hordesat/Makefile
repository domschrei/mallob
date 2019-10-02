WSPACE = 	..

CXXFLAGS =	-O3 -Wall -fmessage-length=0 -I$(WSPACE)/minisat -I$(WSPACE)/lingeling -D __STDC_LIMIT_MACROS -D __STDC_FORMAT_MACROS
#CXXFLAGS =	-O0 -g -Wall -fmessage-length=0 -I$(WSPACE)/minisat -I$(WSPACE)/lingeling -D __STDC_LIMIT_MACROS -D __STDC_FORMAT_MACROS

#OBJS =		hordesat.o
#OBJS =		tests/longestpath.o
#OBJS =		tests/genipaessentials.o
OBJS =		HordeLib.o utilities/mympi.o utilities/ClauseFilter.o solvers/MiniSat.o solvers/Lingeling.o \
		utilities/ClauseDatabase.o ipahorde.o \
		utilities/BufferManager.o utilities/ClauseManager.o utilities/SatUtils.o utilities/Logger.o \
		sharing/AllToAllSharingManager.o sharing/LogSharingManager.o sharing/AsyncRumorSharingManager.o

LIBS =		-lz -L$(WSPACE)/minisat/build/release/lib -lminisat -L$(WSPACE)/lingeling/ -llgl -lpthread

#TARGET =	hordesat
#TARGET =	dimspec
#TARGET = essentials
#TARGET = incremental
TARGET = libhordesat.a

#CXX = 		g++
CXX =		mpic++ 

dimspec:	$(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) tests/dimspec.o $(OBJS) $(LIBS)

hordesat.a:	$(OBJS)
	#ar rcs libhordesat.a $(OBJS)
	ar rvs libhordesat.a $(OBJS)
	
	
all:	$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
