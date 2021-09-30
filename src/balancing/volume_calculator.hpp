
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
    int _epoch;
    int _num_workers;
    int _verbosity;

public:
    VolumeCalculator(const EventMap& events, Parameters& params, int numWorkers, int verbosity) : 
            _params(params), _epoch(events.getGlobalEpoch()), _num_workers(numWorkers), _verbosity(verbosity) {

        // For each event
        log(_verbosity, "BLC Collecting %i entries\n", events.getEntries().size());
        _entries.reserve(events.getEntries().size());
        for (const auto& [jobId, ev] : events.getEntries()) {
            assert(ev.demand >= 0);
            if (ev.demand == 0) continue; // job has no demand
            assert((ev.priority > 0) || log_return_false("#%i has priority %.2f!\n", ev.jobId, ev.priority));

            _entries.push_back(BalancingEntry(ev.jobId, ev.demand, ev.priority));
        }
    }

    void calculateResult() {
        calculateFunctionOptimizationAssignments();
    }

    const std::vector<BalancingEntry>& getEntries() {
        return _entries;
    }

private:

    struct EntryComparatorByPriority {
        bool operator()(const BalancingEntry& first, const BalancingEntry& second) const {
            if (first.priority != second.priority) return first.priority > second.priority;
            auto firstHash = robin_hood::hash_int(first.jobId);
            auto secondHash = robin_hood::hash_int(second.jobId);
            if (firstHash != secondHash) return firstHash < secondHash;
            return first.jobId < second.jobId;
        }
    };

    int round(double value, double cutoff) {
        int floor = (int)value;
        return floor + (value-floor >= cutoff ? 1 : 0);
    }

    void calculateFunctionOptimizationAssignments() {

        int availableVolume = _num_workers * _params.loadFactor();

        if (availableVolume - _entries.size() < 0) {
            log(_verbosity, "BLC too many jobs, bailing out\n");
            return;
        }

        log(_verbosity, "BLC Sorting entries\n");
        std::sort(_entries.begin(), _entries.end(), EntryComparatorByPriority());

        log(_verbosity, "BLC Computing fair shares\n");

        double sumOfPriorities = 0;
        for (auto& job : _entries) sumOfPriorities += job.priority;

        // If floating-point assignments are rounded to integer volumes,
        // use a certain interval [a,b] (0 < a < 0.5 < b < 1, b-0.5 = 0.5-a)
        // in which the job's "rank" will decide whether to round up or down.
        // This breaks ties between jobs that are otherwise identical.
        constexpr float roundingTieBreakerInterval = 0.5;
        constexpr double tieBreakerBegin = 0.5 - 0.5*roundingTieBreakerInterval;
        double tieBreakerStep = roundingTieBreakerInterval / _entries.size();
        
        // Calculate fair share of resources for each job
        int sumOfDemands = 0;
        double minMultiplier = INT32_MAX;
        double maxMultiplier = 0;
        double centerOfWeight = 0;
        double tieBreaker = tieBreakerBegin;
        for (auto& job : _entries) {
            job.fairShare = job.priority / sumOfPriorities * availableVolume;
            job.demand = std::min(job.demand, (int) (availableVolume - _entries.size() + 1)); // cap demand at max. reachable volume
            job.remainder = tieBreaker;
            auto lb = job.getFairShareMultiplierLowerBound();
            auto ub = job.getFairShareMultiplierUpperBound();
            centerOfWeight += (lb+ub)/2;
            minMultiplier = std::min(minMultiplier, lb);
            maxMultiplier = std::max(maxMultiplier, ub);
            sumOfDemands += job.demand;
            tieBreaker += tieBreakerStep;

            assert(std::abs(1 - job.getVolume(1 / job.fairShare)) <= 0.0001 || log_return_false("%.5f\n", job.getVolume(1 / job.fairShare)));
            assert(std::abs(job.demand - job.getVolume(job.demand / job.fairShare)) <= 0.0001);
            //log(_verbosity, "BLC #%i : fair share %.3f\n", job.jobId, job.fairShare);
        }
        centerOfWeight /= _entries.size();

        if (sumOfDemands <= availableVolume) {
            // Trivial case: every job receives its full demand
            for (auto& job : _entries) job.volume = job.demand;
            return;
        }

        // Non-trivial case: some jobs do not receive their full demand
        // Do root search over possible multipliers for fair share

        // Function to evaluate penalty at a certain multiplier for fair share
        double lastLeft = -1;
        double lastRight = -1;
        int numDismissedJobs = 0;
        int baseUtilization = 0;
        auto calcExcessVolume = [&](double x, double left, double right) {
            
            if (lastLeft == -1) {
                // Compute job volumes for left and right as well
                for (auto& job : _entries) {
                    job.volumeLower = job.getVolume(left);
                    job.volumeUpper = job.getVolume(right);
                }
            }

            bool leftHalf = left == lastLeft;
            bool rightHalf = right == lastRight;
            int utilization = baseUtilization;
            double fairShareMultiplier = x;

            for (size_t i = numDismissedJobs; i < _entries.size(); i++) {
                auto& job = _entries[i];
                
                if (leftHalf) job.volumeUpper = job.volume;
                if (rightHalf) job.volumeLower = job.volume;
                
                if (job.volumeLower == job.volumeUpper) {
                    // Job has the same volume in the entire range to search:
                    // dismiss job from remaining computation
                    //log(_verbosity, "f_j=%.3f r_j=%.3f\n", job.fairShare, job.remainder);
                    //log(_verbosity, "#%i : v(%.3f) = v(%.3f) = %i. Dismissing\n", job.jobId, left, right, job.volumeLower);
                    utilization += job.volumeLower;
                    baseUtilization += job.volumeLower;
                    job.dismissAndSwapWith(_entries[numDismissedJobs++]);
                } else {
                    // Evaluate job's volume, add to utilization
                    job.volume = job.getVolume(fairShareMultiplier);
                    utilization += job.volume;
                }
            }

            log(_verbosity, "BLC util @ multiplier %.6f : %i (%i jobs left)\n", fairShareMultiplier, utilization, _entries.size()-numDismissedJobs);
            lastLeft = left;
            lastRight = right;

            return availableVolume - utilization;
        };

        // -- basic bisection method
        
        double lower = minMultiplier; // Only assignments of "1" are made -> less jobs than workers -> UNDERutilization
        double upper = maxMultiplier; // All demands are fully assigned -> OVERutilization or best you can do
        double mid = centerOfWeight;
        double best;
        log(_verbosity, "BLC Finding opt. multiplier, starting range [%.4f, %.4f]\n", lower, upper);

        int bestExcess = -1;
        {
            int excess;
            while (true) {
                excess = calcExcessVolume(mid, lower, upper);

                // Update best excess found so far, if necessary
                if (bestExcess == -1 || std::abs(excess) < std::abs(bestExcess)) {
                    best = mid;
                    bestExcess = excess;
                    if (bestExcess == 0) {
                        break; // optimal excess found
                    }
                }
                
                if (excess > 0) {
                    // unused resources left: increase multiplier
                    lower = mid;
                }
                if (excess < 0) {
                    // too many resources used: decrease multiplier
                    upper = mid;
                }

                // Update point of evaluation
                mid = (lower+upper)*0.5;
            }
        }

        // "Perfect" multiplier to use
        double fairShareMultiplier = best;
        log(_verbosity, "BLC found multiplier %.6f (excess %i)\n", best, (int)bestExcess);
    }
};

#endif
