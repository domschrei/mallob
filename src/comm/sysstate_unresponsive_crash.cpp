
#include "sysstate.hpp"

bool unresponsiveNodeCrashingEnabled {true};

bool SysState_isUnresponsiveNodeCrashingEnabled() {return unresponsiveNodeCrashingEnabled;}
void SysState_disableUnresponsiveNodeCrashing() {unresponsiveNodeCrashingEnabled = false;}
