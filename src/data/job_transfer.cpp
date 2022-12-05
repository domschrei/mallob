
#include "job_transfer.hpp"

#include <cstring>
#include <sstream>

#include "data/job_description.hpp"

/*static!*/ size_t JobRequest::getMaxTransferSize() {
    return 8*sizeof(int)+sizeof(float)+sizeof(bool)
        +4*sizeof(int);
}

size_t JobRequest::getTransferSize() const {
    return 8*sizeof(int)+sizeof(float)+sizeof(bool)
        +(multiplicity == 1 ? 2 : 4)*sizeof(int);
}

std::vector<uint8_t> JobRequest::serialize() const {
    int size = getTransferSize();
    std::vector<uint8_t> packed(size);
    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &applicationId, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &rootRank, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &requestingNodeRank, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &requestedNodeIndex, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &revision, n); i += n;
    n = sizeof(float); memcpy(packed.data()+i, &timeOfBirth, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &numHops, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &balancingEpoch, n); i += n;
    n = sizeof(bool); memcpy(packed.data()+i, &incremental, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &multiBaseId, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &multiplicity, n); i += n;
    if (multiplicity == 1) return packed;
    n = sizeof(int); memcpy(packed.data()+i, &multiBegin, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &multiEnd, n); i += n;
    return packed;
}

JobRequest& JobRequest::deserialize(const std::vector<uint8_t> &packed) {
    int i = 0, n;
    n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&applicationId, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&rootRank, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&requestingNodeRank, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&requestedNodeIndex, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&revision, packed.data()+i, n); i += n;
    n = sizeof(float); memcpy(&timeOfBirth, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&numHops, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&balancingEpoch, packed.data()+i, n); i += n;
    n = sizeof(bool); memcpy(&incremental, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&multiBaseId, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&multiplicity, packed.data()+i, n); i += n;
    if (multiplicity == 1) return *this;
    n = sizeof(int); memcpy(&multiBegin, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&multiEnd, packed.data()+i, n); i += n;
    return *this;
}

std::string JobRequest::toStr() const {
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << timeOfBirth;
    auto birthStr = out.str();
    return "r.#" + std::to_string(jobId) + ":" + std::to_string(requestedNodeIndex) 
            + " rev. " + std::to_string(revision) + " <- [" 
            + std::to_string(requestingNodeRank) + "] born=" + birthStr 
            + " hops=" + std::to_string(numHops)
            + " epoch=" + std::to_string(balancingEpoch)
            + " matchId=" + std::to_string(multiBaseId)
            + (multiplicity>1 ? 
                " x" + std::to_string(multiplicity) 
                    + " [" + std::to_string(multiBegin) + "," + std::to_string(multiEnd) + "]" 
                : "");
}

bool JobRequest::operator==(const JobRequest& other) const {
    return jobId == other.jobId 
        && requestedNodeIndex == other.requestedNodeIndex 
        && balancingEpoch == other.balancingEpoch
        && revision == other.revision
        && numHops == other.numHops;
}
bool JobRequest::operator!=(const JobRequest& other) const {
    return !(*this == other);
}
bool JobRequest::operator<(const JobRequest& other) const {
    if (balancingEpoch != other.balancingEpoch) return balancingEpoch < other.balancingEpoch;
    if (jobId != other.jobId) return jobId < other.jobId;
    if (requestedNodeIndex != other.requestedNodeIndex) return requestedNodeIndex < other.requestedNodeIndex;
    if (revision != other.revision) return revision < other.revision;
    return false;
}
    
std::vector<uint8_t> OneshotJobRequestRejection::serialize() const {
    std::vector<uint8_t> packed = request.serialize();
    size_t sizeBefore = packed.size();
    packed.resize(packed.size()+sizeof(bool));
    memcpy(packed.data()+sizeBefore, &isChildStillDormant, sizeof(bool));
    return packed;
}

OneshotJobRequestRejection& OneshotJobRequestRejection::deserialize(const std::vector<uint8_t> &packed) {
    request.deserialize(packed);
    memcpy(&isChildStillDormant, packed.data()+packed.size()-sizeof(bool), sizeof(bool));
    return *this;
}

std::vector<uint8_t> WorkRequest::serialize() const {
    std::vector<uint8_t> packed(3*sizeof(int));
    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &requestingRank, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &numHops, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &balancingEpoch, n); i += n;
    return packed;
}

WorkRequest& WorkRequest::deserialize(const std::vector<uint8_t> &packed) {
    int i = 0, n;
    n = sizeof(int); memcpy(&requestingRank, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&numHops, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&balancingEpoch, packed.data()+i, n); i += n;
    return *this;
}

bool WorkRequestComparator::operator()(const WorkRequest& lhs, const WorkRequest& rhs) const {
    if (lhs.balancingEpoch != rhs.balancingEpoch) return lhs.balancingEpoch > rhs.balancingEpoch;
    if (lhs.numHops != rhs.numHops) return lhs.numHops < rhs.numHops;
    return std::hash<int>()(lhs.requestingRank) < std::hash<int>()(rhs.requestingRank);
}

int JobSignature::getTransferSize() const {
    return transferSize;
}

std::vector<uint8_t> JobSignature::serialize() const {
    int size = (3*sizeof(int) + sizeof(size_t));
    std::vector<uint8_t> packed(size);

    int i = 0, n;
    n = sizeof(int);    memcpy(packed.data()+i, &jobId, n); i += n;
    n = sizeof(int);    memcpy(packed.data()+i, &rootRank, n); i += n;
    n = sizeof(int);    memcpy(packed.data()+i, &firstIncludedRevision, n); i += n;
    n = sizeof(size_t); memcpy(packed.data()+i, &transferSize, n); i += n;
    return packed;
}

JobSignature& JobSignature::deserialize(const std::vector<uint8_t>& packed) {
    int i = 0, n;
    n = sizeof(int);    memcpy(&jobId, packed.data()+i, n); i += n;
    n = sizeof(int);    memcpy(&rootRank, packed.data()+i, n); i += n;
    n = sizeof(int);    memcpy(&firstIncludedRevision, packed.data()+i, n); i += n;
    n = sizeof(size_t); memcpy(&transferSize, packed.data()+i, n); i += n;
    return *this;
}

std::vector<uint8_t> JobMessage::serialize() const {
    int size = 4*sizeof(int) + sizeof(bool) + payload.size()*sizeof(int) + sizeof(Checksum);
    std::vector<uint8_t> packed(size);

    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &jobId, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &revision, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &tag, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &epoch, n); i += n;
    n = sizeof(bool); memcpy(packed.data()+i, &returnedToSender, n); i += n;
    n = sizeof(Checksum); memcpy(packed.data()+i, &checksum, n); i += n;
    n = payload.size()*sizeof(int); memcpy(packed.data()+i, payload.data(), n); i += n;
    return packed;
}

JobMessage& JobMessage::deserialize(const std::vector<uint8_t>& packed) {
    int i = 0, n;
    n = sizeof(int); memcpy(&jobId, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&revision, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&tag, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&epoch, packed.data()+i, n); i += n;
    n = sizeof(bool); memcpy(&returnedToSender, packed.data()+i, n); i += n;
    n = sizeof(Checksum); memcpy(&checksum, packed.data()+i, n); i += n;
    n = packed.size()-i; payload.resize(n/sizeof(int)); 
    memcpy(payload.data(), packed.data()+i, n); i += n;
    return *this;
}

std::vector<uint8_t> IntPair::serialize() const {
    int size = (2*sizeof(int));
    std::vector<uint8_t> packed(size);
    int i = 0, n;
    n = sizeof(int); memcpy(packed.data()+i, &first, n); i += n;
    n = sizeof(int); memcpy(packed.data()+i, &second, n); i += n;
    return packed;
}

IntPair& IntPair::deserialize(const std::vector<uint8_t>& packed) {
    int i = 0, n;
    n = sizeof(int); memcpy(&first, packed.data()+i, n); i += n;
    n = sizeof(int); memcpy(&second, packed.data()+i, n); i += n;
    return *this;
}

std::vector<uint8_t> IntVec::serialize() const {
    int size = (data.size()*sizeof(int));
    std::vector<uint8_t> packed(size);
    memcpy(packed.data(), data.data(), size);
    return packed;
}

IntVec& IntVec::deserialize(const std::vector<uint8_t>& packed) {
    data.resize(packed.size() / sizeof(int));
    memcpy(data.data(), packed.data(), packed.size());
    return *this;
}

int& IntVec::operator[](const int pos) {
    return data[pos];   
}

std::vector<uint8_t> JobStatistics::serialize() const {
    std::vector<uint8_t> packed(3*sizeof(int) + 3*sizeof(float));
    int i = 0, n;
    n = sizeof(int);   memcpy(packed.data()+i, &jobId, sizeof(int));                      i += n;
    n = sizeof(int);   memcpy(packed.data()+i, &revision, sizeof(int));                   i += n;
    n = sizeof(int);   memcpy(packed.data()+i, &successfulRank, sizeof(int));             i += n;
    n = sizeof(float); memcpy(packed.data()+i, &usedWallclockSeconds, sizeof(float));     i += n;
    n = sizeof(float); memcpy(packed.data()+i, &usedCpuSeconds, sizeof(float));           i += n;
    n = sizeof(float); memcpy(packed.data()+i, &latencyOf1stVolumeUpdate, sizeof(float)); i += n;
    return packed;
}

JobStatistics& JobStatistics::deserialize(const std::vector<uint8_t>& packed) {
    int i = 0, n;
    n = sizeof(int);   memcpy(&jobId, packed.data()+i, sizeof(int));                      i += n;
    n = sizeof(int);   memcpy(&revision, packed.data()+i, sizeof(int));                   i += n;
    n = sizeof(int);   memcpy(&successfulRank, packed.data()+i, sizeof(int));             i += n;
    n = sizeof(float); memcpy(&usedWallclockSeconds, packed.data()+i, sizeof(float));     i += n;
    n = sizeof(float); memcpy(&usedCpuSeconds, packed.data()+i, sizeof(float));           i += n;
    n = sizeof(float); memcpy(&latencyOf1stVolumeUpdate, packed.data()+i, sizeof(float)); i += n;
    return *this;
}
