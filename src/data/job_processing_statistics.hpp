
#pragma once

struct JobProcessingStatistics {
    float timeOfSubmission {0};
    float timeOfScheduling {0};
    float parseTime {0};
    float schedulingTime {0};
    float processingTime {0};
    float totalResponseTime {0};
    float usedWallclockSeconds {0};
    float usedCpuSeconds {0};
    float latencyOf1stVolumeUpdate {0};
};
