
#include "core_allocator.hpp"

CoreAllocator* ProcessWideCoreAllocator::ca;

CoreAllocator::Allocation::Allocation(int nbRequested) {
    granted = ProcessWideCoreAllocator::get().requestCores(nbRequested);
}
int CoreAllocator::Allocation::requestCores(int nbRequested) {
    int nbGranted = ProcessWideCoreAllocator::get().requestCores(nbRequested);
    granted += nbGranted;
    return nbGranted;
}
void CoreAllocator::Allocation::returnCores(int nbReturned) {
    assert(nbReturned <= granted);
    ProcessWideCoreAllocator::get().returnCores(nbReturned);
    granted -= nbReturned;
}
void CoreAllocator::Allocation::returnAllCores() {
    returnCores(granted);
}
CoreAllocator::Allocation::~Allocation() {
    returnAllCores();
}
