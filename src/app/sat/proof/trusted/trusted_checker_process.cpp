
#include "trusted_checker_process.hpp"
#include "trusted_utils.hpp"

void logInCheckerProcess(void*, const char* msg) {
    TrustedUtils::log(msg);
}

int main(int argc, char *argv[]) {
    const char optDirectives[] = "-fifo-directives=";
    const char optFeedback[] = "-fifo-feedback=";
    const char* fifoDirectives;
    const char* fifoFeedback;
    for (int i = 0; i < argc; i++) {
        if (TrustedUtils::beginsWith(argv[i], optDirectives)) {
            fifoDirectives = argv[i] + (sizeof(optDirectives)-1);
        }
        if (TrustedUtils::beginsWith(argv[i], optFeedback)) {
            fifoFeedback = argv[i] + (sizeof(optFeedback)-1);
        }
    }
    TrustedUtils::log("Using input path", fifoDirectives);
    TrustedUtils::log("Using output path", fifoFeedback);
    TrustedCheckerProcess p(fifoDirectives, fifoFeedback);
    return p.run();
}
