
#ifndef HORDE_MALLOB_SOLVING_STATE_H
#define HORDE_MALLOB_SOLVING_STATE_H

namespace SolvingStates {

	enum SolvingState {
		INITIALIZING, ACTIVE, SUSPENDED, STANDBY, ABORTING
	};
	extern const char* SolvingStateNames[];
}

#endif