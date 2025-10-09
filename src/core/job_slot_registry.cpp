
#include "job_slot_registry.hpp"

std::set<std::shared_ptr<JobSlotRegistry::JobSlot>, JobSlotRegistry::JobSlotCompare> JobSlotRegistry::_slots;
int JobSlotRegistry::_max_nb_slots = 0;
