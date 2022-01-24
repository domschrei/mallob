
#include "comm/mpi_base.hpp"

#include "util/logger.hpp"

void chkerr(int err) {
    if (err != 0) {
        LOG(V0_CRIT, "[ERROR] MPI errcode=%i\n", err);
        Logger::getMainInstance().flush();
        abort();
    }
}
