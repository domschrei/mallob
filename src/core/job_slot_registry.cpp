
#include "job_slot_registry.hpp"

std::list<JobSlotRegistry::JobSlot*> JobSlotRegistry::_slots;
int JobSlotRegistry::_max_nb_slots = 0;
