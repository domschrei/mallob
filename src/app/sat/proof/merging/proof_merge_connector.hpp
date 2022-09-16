
#pragma once

#include "../serialized_lrat_line.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"

typedef SPSCBlockingRingbuffer<SerializedLratLine> ProofMergeConnector;
