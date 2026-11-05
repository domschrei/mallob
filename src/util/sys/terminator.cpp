
#include "terminator.hpp"
#include "util/periodic_event.hpp"

std::atomic_bool Terminator::_exit = false;
bool Terminator::_forward_terminate_to_children = true;
PeriodicEvent<100> Terminator::_check_terminate_file;
