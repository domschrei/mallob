
#include <cstdio>
#include <sys/prctl.h>

#include "trusted_checker_process.hpp"
#include "trusted_utils.hpp"

int main(int argc, char *argv[]) {

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    const char optDirectives[] = "-fifo-directives=";
    const char optFeedback[] = "-fifo-feedback=";
    const char* fifoDirectives;
    const char* fifoFeedback;
    for (int i = 0; i < argc; i++) {
        if (TrustedUtils::beginsWith(argv[i], optDirectives))
            fifoDirectives = argv[i] + (sizeof(optDirectives)-1);
        if (TrustedUtils::beginsWith(argv[i], optFeedback))
            fifoFeedback = argv[i] + (sizeof(optFeedback)-1);
    }
    TrustedUtils::log("Using input path", fifoDirectives);
    TrustedUtils::log("Using output path", fifoFeedback);
    TrustedCheckerProcess p(fifoDirectives, fifoFeedback);
    int res = p.run();
    fflush(stdout);
    return res;
}
