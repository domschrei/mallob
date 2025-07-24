
#include "util/sys/process.hpp"
#include "util/sys/process_dispatcher.hpp"

int main() {
    Process::init(0);
    ProcessDispatcher().dispatch();
}
