
#include "stxxl.hpp"

#include <stxxl.h>

namespace mallob_stxxl {
    void init(int rank, const std::string& diskLocation, int numGigabytes) {
        // get uninitialized config singleton
        stxxl::config* cfg = stxxl::config::get_instance();
        // create a disk_config structure.
        std::string configStr = "disk=" + diskLocation + "/~stxxl." + std::to_string(rank) 
            + "," + std::to_string(numGigabytes) + "G,syscall unlink";
        stxxl::disk_config disk(configStr);
        //disk.direct = stxxl::disk_config::DIRECT_ON; // force O_DIRECT
        // add disk to config
        cfg->add_disk(disk);
    }
}
