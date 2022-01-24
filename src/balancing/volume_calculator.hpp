
#ifndef DOMPASCH_MALLOB_VOLUME_CALCULATOR_HPP
#define DOMPASCH_MALLOB_VOLUME_CALCULATOR_HPP

#include <string>
#include <vector>
#include <cmath>
#include <list>
#include <algorithm>

#include "util/assert.hpp"
#include "util/params.hpp"
#include "balancing/event_map.hpp"
#include "balancing/balancing_entry.hpp"
//#include "util/math/chandrupatla.hpp"

class VolumeCalculator {

private:
    Parameters& _params;
    std::vector<BalancingEntry> _entries;
    std::vector<BalancingEntry> _zero_entries;
    int _epoch;
    int _num_workers;
    bool _logging;

    double _sum_of_priorities = 0;
    int _available_volume = 0;
    unsigned long long _sum_of_demands = 0;

    double _min_multiplier;
    double _max_multiplier;
    double _center_of_mass_of_multiplier;

    double _prev_lb;
    double _prev_ub;
    int _num_dismissed_jobs = 0;
    int _base_utilization;
    int _max_volume_diff_between_bounds = 0;

public:
    VolumeCalculator(const EventMap& events, Parameters& params, int numWorkers, bool logging) : 
            _params(params), _epoch(events.getGlobalEpoch()), _num_workers(numWorkers),
            _logging(logging) {

        // For each event
        if (_logging) LOG(V5_DEBG, "BLC Collecting %i entries\n", events.getEntries().size());
        _entries.reserve(events.getEntries().size());
        for (const auto& [jobId, ev] : events.getEntries()) {
            assert(ev.demand >= 0);
            if (ev.demand == 0) _zero_entries.emplace_back(ev.jobId, ev.demand, ev.priority); // job has no demand
            else {
                assert((ev.priority > 0) || LOG_RETURN_FALSE("#%i has priority %.2f!\n", ev.jobId, ev.priority));
                _entries.emplace_back(ev.jobId, ev.demand, ev.priority);
                _sum_of_priorities += ev.priority;
            }
        }

        _available_volume = _num_workers * _params.loadFactor();
    }

    void calculateResult() {

        // Check if there are enough workers for the active jobs
        if (_logging) LOG(V5_DEBG, "BLC #av=%i #j=%i\n", _available_volume, _entries.size());
        if (_available_volume <= _entries.size()) {
            if (_logging) LOG(V5_DEBG, "BLC too many jobs, bailing out\n");
            return;
        }

        sortRemainingEntries();
        computeFairShares();

        // Trivial case: every job receives its full demand
        if (_sum_of_demands <= _available_volume) {
            for (auto& job : _entries) job.volume = job.demand;
            return;
        }

        // Non-trivial case: some jobs do not receive their full demand.
        // Do root search over possible multipliers for fair share
        calculateFunctionOptimizationAssignments();
    }

    const std::vector<BalancingEntry>& getEntries() {
        return _entries;
    }
    const std::vector<BalancingEntry>& getZeroEntries() {
        return _zero_entries;
    }

private:

    struct EntryComparatorByPriority {
        bool operator()(const BalancingEntry& first, const BalancingEntry& second) const {
            // Highest priority first
            if (first.priority != second.priority) 
                return first.priority > second.priority;
            // Highest demand first
            if (first.originalDemand != second.originalDemand) 
                return first.originalDemand > second.originalDemand;
            // Break ties pseudo-randomly (yet deterministically) via hash of job ID
            auto firstHash = robin_hood::hash_int(first.jobId);
            auto secondHash = robin_hood::hash_int(second.jobId);
            if (firstHash != secondHash) return firstHash < secondHash;
            // Break ties via job ID
            return first.jobId < second.jobId;
        }
    };

    void sortRemainingEntries() {
        std::sort(_entries.begin()+_num_dismissed_jobs, _entries.end(), EntryComparatorByPriority());
    }

    void computeFairShares() {

        const static double EPSILON = 1e-6;

        // Calculate fair share of resources for each job
        _min_multiplier = INT32_MAX;
        _max_multiplier = 0;
        _center_of_mass_of_multiplier = 0;

        for (auto& job : _entries) {
            job.fairShare = job.priority / _sum_of_priorities * _available_volume;
            job.demand = std::min(job.demand, (int) (_available_volume - _entries.size() + 1)); // cap demand at max. reachable volume
            assert(job.demand > 0);
            auto lb = job.getFairShareMultiplierLowerBound();
            auto ub = job.getFairShareMultiplierUpperBound();
            //log(_verbosity+1, "BLCPLOT %i %.6f %.6f %i\n", job.jobId, lb, ub, job.demand);
            _center_of_mass_of_multiplier += (lb+ub)/2;
            _min_multiplier = std::min(_min_multiplier, lb);
            _max_multiplier = std::max(_max_multiplier, ub);
            _sum_of_demands += job.demand;

            assert(1 == job.getVolume(1 / job.fairShare) || LOG_RETURN_FALSE("ERROR #%i 1 != %i\n", job.jobId, job.getVolume(1 / job.fairShare)));
            assert(job.demand == job.getVolume(job.demand / job.fairShare + EPSILON) 
                || LOG_RETURN_FALSE("ERROR #%i %i != %i\n", job.jobId, job.demand, job.getVolume(job.demand / job.fairShare)));
            if (_logging) LOG(V6_DEBGV, "BLC #%i : fair share %.3f\n", job.jobId, job.fairShare);
        }

        _max_multiplier += EPSILON;
        _center_of_mass_of_multiplier /= _entries.size();
    }

    void calculateFunctionOptimizationAssignments() {

        _prev_lb = -1;
        _prev_ub = -1;
        _num_dismissed_jobs = 0;
        _base_utilization = 0;
        _max_volume_diff_between_bounds = 0;
        
        double lower = _min_multiplier; // Only assignments of "1" are made -> less jobs than workers -> UNDERutilization
        double upper = _max_multiplier; // All demands are fully assigned -> OVERutilization or best you can do
        double mid = 1; // Except for pathological cases, a factor of 1 is a pretty good initialization
        double best;
        if (_logging) LOG(V5_DEBG, "BLC Finding opt. multiplier, starting range [%.4f, %.4f]\n", lower, upper);

        int bestExcess = -1;
        int excessAtLeft = _available_volume - _entries.size(); // needed for base case
        int numIterations = 0;
        while (true) {

            numIterations++;
            int excess = calculateExcessVolumeOrNegative(mid, lower, upper);

            // Update best excess found so far, if necessary
            if (bestExcess == -1 || std::abs(excess) < std::abs(bestExcess)) {
                best = mid;
                bestExcess = excess;
                if (bestExcess == 0) {
                    break; // optimal excess found
                }
            }
            
            // Base case condition met?
            if (_max_volume_diff_between_bounds <= 1) {
                
                // Each job's volume differs by at most one in between the current bounds.
                // At the lower bound, assign 1 additional worker to each job f.l.t.r.
                // until the utilization is optimal.
            
                best = lower;
                excess = excessAtLeft;
                int numRemainingJobs = _entries.size() - _num_dismissed_jobs;


                assert(numRemainingJobs > 0);
                int stillAvailable = excess;
                assert(stillAvailable > 0);
                
                if (_logging) LOG(V4_VVER, "BLC FINALIZE %.6f it=%i excess=%i, V=%i J=%i\n", 
                    best, numIterations, excess, stillAvailable, numRemainingJobs);
                
                // Sort the remaining jobs here because they may have been
                // shuffled by dismissed jobs in earlier iterations.
                // TODO QuickSelect could be used instead to find the "threshold job"
                sortRemainingEntries();

                for (size_t i = _num_dismissed_jobs; i < _entries.size(); i++) {
                    auto& job = _entries[i];
                    job.volume = job.volumeLower;
                    if (stillAvailable > 0) {
                        job.volume++;
                        stillAvailable--;
                    }
                }

                assert(stillAvailable == 0);
                bestExcess = 0;
                break;
            }

            if (excess > 0) {
                // unused resources left: increase multiplier
                lower = mid;
                excessAtLeft = excess;
                //mid = std::min((lower+upper)*0.5, 2*mid);
            }
            if (excess < 0) {
                // too many resources used: decrease multiplier
                upper = mid;
            }

            if (mid == 1 && excess > 0 && _center_of_mass_of_multiplier > mid) {
                mid = _center_of_mass_of_multiplier;
            } else if (mid == 1 && excess < 0 && _center_of_mass_of_multiplier < mid) {
                mid = _center_of_mass_of_multiplier;
            } else {
                mid = (lower+upper)/2;
            }
        }

        // "Perfect" multiplier to use
        double fairShareMultiplier = best;
        if (_logging) LOG(V4_VVER, "BLC FINALIZED alpha=%.6f excess=%i\n", best, (int)bestExcess);
    }

    int calculateExcessVolumeOrNegative(double fairShareMultiplier, double left, double right) {
            
        if (_prev_lb == -1) {
            // Compute job volumes for left and right as well
            for (auto& job : _entries) {
                job.volumeLower = job.getVolume(left);
                job.volumeUpper = job.getVolume(right);
            }
        }

        // Remember which half we are searching, to re-use cached volumes
        bool leftHalf = left == _prev_lb;
        bool rightHalf = right == _prev_ub;

        int utilization = _base_utilization;
        _max_volume_diff_between_bounds = 0;

        for (size_t i = _num_dismissed_jobs; i < _entries.size(); i++) {
            auto& job = _entries[i];
            
            // Cache last volume result at one of the bounds
            if (leftHalf) job.volumeUpper = job.volume;
            if (rightHalf) job.volumeLower = job.volume;
            
            if (job.volumeLower == job.volumeUpper) {
                // Job has the same volume in the entire range to search:
                // dismiss job from remaining computation
                //log(_verbosity, "BLC #%i : f_j=%.3f\n", job.jobId, job.fairShare);
                //log(_verbosity, "BLC #%i : v(%.3f) = v(%.3f) = %i. Dismissing\n", job.jobId, left, right, job.volumeLower);
                job.volume = job.volumeLower;
                utilization += job.volumeLower;
                _base_utilization += job.volumeLower;
                job.dismissAndSwapWith(_entries[_num_dismissed_jobs++]);
            } else {
                // Evaluate job's volume, add to utilization
                job.volume = job.getVolume(fairShareMultiplier);
                //log(_verbosity, "BLC #%i : f_j=%.3f v_j=%i\n", job.jobId, job.fairShare, job.volume);
                // overflow protection: do not compute utilization beyond a point
                // where it is already too high
                if (utilization <= _available_volume) 
                    utilization += job.volume;
                _max_volume_diff_between_bounds = std::max(_max_volume_diff_between_bounds, job.volumeUpper - job.volumeLower);
            }
        }

        if (_logging) LOG(V5_DEBG, "BLC util @ multiplier %.6f : %i (%i jobs left)\n", fairShareMultiplier, utilization, 
            _entries.size()-_num_dismissed_jobs);
        _prev_lb = left;
        _prev_ub = right;

        return _available_volume - utilization;
    }
};

#endif
