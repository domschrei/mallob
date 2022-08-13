
#include "stxxl.hpp"

#include <stxxl.h>

namespace mallob_stxxl {
    void init(int rank) {
        // get uninitialized config singleton
        stxxl::config* cfg = stxxl::config::get_instance();
        // create a disk_config structure.
        stxxl::disk_config disk("disk=./~stxxl." + std::to_string(rank) + ",256G,syscall unlink");
        //disk.direct = stxxl::disk_config::DIRECT_ON; // force O_DIRECT
        // add disk to config
        cfg->add_disk(disk);
    }
}
