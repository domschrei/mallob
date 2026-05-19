
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/process_dispatcher.hpp"

int main() {
    //Proc::closeAllFileDescriptors();
    Process::init(0);
    ProcessDispatcher().dispatch();
}
