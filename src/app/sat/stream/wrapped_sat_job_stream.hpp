
#pragma once

#include "sat_job_stream.hpp"
#include "app/sat/stream/mallob_sat_job_stream_processor.hpp"

struct WrappedSatJobStream {
    SatJobStream stream;
    std::list<MallobSatJobStreamProcessor*> mallobProcessors;
    std::atomic<void*> innerTerminator {nullptr};
    WrappedSatJobStream(const std::string& name) : stream(name) {}
};
