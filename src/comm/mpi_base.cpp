
#include "comm/mpi_base.hpp"

#include "util/logger.hpp"

void chkerr(int err) {
    if (err != 0) {
        log(V0_CRIT, "MPI ERROR errcode=%i\n", err);
        Logger::getMainInstance().flush();
        abort();
    }
}
