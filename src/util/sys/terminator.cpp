
#include "terminator.hpp"

std::atomic_bool Terminator::_exit = false;
bool Terminator::_forward_terminate_to_children = true;
