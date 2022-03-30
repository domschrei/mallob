
#pragma once

namespace SolvingStates {

	enum SolvingState {
		INITIALIZING, ACTIVE, SUSPENDED, STANDBY, ABORTING
	};
	extern const char* SolvingStateNames[];
}
