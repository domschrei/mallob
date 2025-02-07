
#pragma once

#include <climits>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <mutex>
#include <queue>
#include <set>
#include <string>

#include "util/assert.hpp"
#include "util/logger.hpp"

class IntervalSearch {

public:
    static constexpr double WEIGHT_SUM = (1<<20);

private:
    // Represents an interval [lb,ub] corresponding to an active (or orphaned) SAT call
    // for cost values <= ub. The interval cannot be empty and contains both lb and ub.
    struct SearchInterval {
        size_t lb;
        size_t ub;
        double weight;
        bool orphaned {false};
        SearchInterval(size_t lb, size_t ub, double weight) : lb(lb), ub(ub), weight(weight) {
            assert(ub >= lb);
            assert(size() > 0);
        }
        size_t size() const {return ub-lb+1;} // inclusive bounds at both ends
        std::pair<SearchInterval, SearchInterval> split(double skew) const {
            assert(size() >= 2);
            if (skew <= 0 || skew >= 1) skew = 0.5;
            size_t mid = lb + (ub-lb) * skew;
            // failsafe to mitigate floating-point issues for small intervals and very large bounds
            if (lb > mid || mid+1 > ub) mid = lb + (size() <= 2 ? 0 : size()/2);
            LOG(V5_DEBG, "COMBSEARCH SPLIT [%lu,%lu] => [%lu,%lu][%lu,%lu]\n", lb, ub, lb, mid, mid+1, ub);
            SearchInterval left {lb, mid, 0.5*weight};
            SearchInterval right {mid+1, ub, 0.5*weight};
            assert(left.size() > 0 && right.size() > 0);
            return {left, right};
        }
        std::pair<SearchInterval, SearchInterval> splitAt(double skew, size_t mid) const {
            double absRatio = (mid - lb + 1) / (double)size();
            double weightRatio = std::pow(skew, std::log(absRatio) / std::log(0.5));
            SearchInterval left {lb, mid, weightRatio * weight};
            SearchInterval right {mid+1, ub, (1 - weightRatio) * weight};
            return {left, right};
        }
        std::string toStr() const {
            return "[" + std::to_string(lb) + ":" + std::to_string(ub) + "]";
        }
    };

    // Invariant: Each interval in here represents an active search call, where
    // the interval's upper bound is the search call's upper bound, EXCEPT for
    // intervals in which "orphaned" is set.
    std::list<SearchInterval> _current_bounds;
    double _skew {0.9};

    std::list<size_t> _bounds_to_replay; // for debugging

public:
    IntervalSearch(double skew) : _skew(skew) {}

    void init(size_t minToTest, size_t maxToTest) {
        // Create first interval with full weight
        _current_bounds.push_back({minToTest, maxToTest, WEIGHT_SUM});
        _current_bounds.back().orphaned = true;
        print();
    }

    bool getNextBound(size_t& b) {
        if (!_bounds_to_replay.empty()) {
            b = _bounds_to_replay.front();
            _bounds_to_replay.pop_front();
            return true;
        }

        if (_current_bounds.empty()) return false;
        std::list<SearchInterval>::iterator best = _current_bounds.begin();
        {
            auto it = best; ++it;
            while (it != _current_bounds.end()) {
                if (best == _current_bounds.end() // first proper interval
                        || (it->orphaned) // prefer rightmost orphaned interval
                        || (!best->orphaned &&
                            ((best->size() == 1 && it->size() > 1) // first interval of size >1
                            || (it->size() > 1 && ( // only accept subsequent intervals of size >1
                                it->weight > best->weight // prefer intervals of high weight
                                || (it->weight == best->weight && it->size() >= best->size()) // tie-break by absolute size
                            )))
                        )
                    ) {
                    best = it;
                }
                ++it;
            }
        }
        if (best->orphaned) {
            best->orphaned = false;
            b = best->ub;
        } else if (best->size() == 1) {
            // nothing to split left
            return false;
        } else {
            SearchInterval bestInt = *best;
            auto [left, right] = bestInt.split(_skew);
            best->lb = right.lb;
            best->ub = right.ub;
            best->weight = right.weight;
            _current_bounds.insert(best, left);
            b = left.ub;
        }
        print();
        return true;
    }
    void stopTestingAndUpdateLower(size_t bound) {
        stopTestingBound(bound, true, false, 0);
    }
    void stopTestingAndUpdateUpper(size_t bound, size_t newBestFoundCost) {
        stopTestingBound(bound, false, true, newBestFoundCost);
    }
    void stopTestingWithoutUpdates(size_t bound) {
        stopTestingBound(bound, false, false, 0);
    }
    void stopTestingBound(size_t bound, bool updateLower, bool updateUpper, size_t newBestFoundCost) {
        if (_current_bounds.empty()) return;
        if (updateUpper) assert(newBestFoundCost <= bound);
        size_t newMax = newBestFoundCost-1; // only for updateUpper
        size_t newMin = bound+1; // only for updateLower
        auto it = _current_bounds.begin();
        double summedWeight {0};
        while (it != _current_bounds.end()) {
            SearchInterval i = *it;
            if (updateUpper && newMax < i.ub) { // does the interval have a right part *outside of* newMax?
                // the upper improvement concerns this interval
                if (newMax < i.lb) {
                    // interval is falling out of the considered range *completely*
                    it = _current_bounds.erase(it);
                } else {
                    // delete all intervals coming afterwards
                    size_t lb = it->lb;
                    while (lb < _current_bounds.back().lb) {
                        _current_bounds.pop_back();
                    }
                    // interval needs to be split so that the cutoff can be deleted
                    auto parent = _current_bounds.back();
                    auto [left, right] = parent.splitAt(_skew, newMax);
                    // erase the interval itself
                    _current_bounds.pop_back();
                    assert(left.ub == newMax);
                    // Mark the new interval as orphaned since there is no active test with this UB.
                    left.orphaned = true;
                    _current_bounds.push_back(left);
                    summedWeight += left.weight;
                    break;
                }
            } else if (updateLower && i.lb < newMin) { // does the interval have a left part *outside of* newMin?
                if (i.ub < newMin) {
                    // The corresponding call stops and the full interval is deleted.
                    it = _current_bounds.erase(it);
                } else {
                    // A strict left-side sub-range of this interval becomes obsolete.
                    // Interval needs to be split so that the cutoff can be deleted.
                    auto [left, right] = i.splitAt(_skew, newMin-1);
                    // delete the interval itself
                    it = _current_bounds.erase(it);
                    // re-insert the right-side sub-range of the interval
                    assert(right.lb == newMin);
                    right.orphaned = i.orphaned; // inherit "orphaned" status
                    it = _current_bounds.insert(it, right);
                    summedWeight += right.weight;
                    ++it;
                }
            } else if (i.ub == bound) {
                // Call is not dominated by a lower or upper bound but still stopped:
                // "stop" the interval *without* shrinking the interval range.
                if (std::next(it) == _current_bounds.end()) {
                    // rightmost interval: mark as orphaned
                    it->orphaned = true;
                    summedWeight += it->weight;
                    ++it;
                } else {
                    // merge interval with the one to the right, if present
                    it = _current_bounds.erase(it);
                    it->lb = i.lb;
                    it->weight += i.weight; // will be summed up next iteration
                }
            } else {
                summedWeight += it->weight;
                ++it;
            }
        }
        // re-normalize weights to counteract accumulating floating-point errors
        for (auto it = _current_bounds.begin(); it != _current_bounds.end(); ++it) {
            it->weight *= WEIGHT_SUM / summedWeight;
        }
        print();
    }
    std::vector<size_t> getActiveSearches() const {
        std::vector<size_t> bounds;
        for (auto i : _current_bounds) {
            if (!i.orphaned) bounds.push_back(i.ub);
        }
        return bounds;
    }
    void print() const {
        std::string out;
        std::string density;
        double weightTotal = 0;
        size_t nextExpectedLb = ULONG_MAX;
        for (auto i : _current_bounds) {
            assert(nextExpectedLb == ULONG_MAX || i.lb == nextExpectedLb);
            assert(i.size() > 0);
            nextExpectedLb = i.ub+1;
            out += (i.orphaned ? "~" : "")
                + std::to_string(i.weight) + "*" + i.toStr()
                + (i.orphaned ? "~ " : " ");
            density += std::to_string(i.weight / i.size()) + " ";
            weightTotal += i.weight;
        }
        LOG(V4_VVER, "COMBSEARCH W=%.3f %s\n", weightTotal, out.c_str());
        LOG(V5_DEBG, "COMBSEARCH DENSITY %s\n", density.c_str());
    }
};
