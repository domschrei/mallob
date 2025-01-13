
#pragma once

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
    struct SearchInterval {
        size_t lb;
        size_t ub;
        double weight;
        bool orphaned {false};
        SearchInterval(size_t lb, size_t ub, double weight) : lb(lb), ub(ub), weight(weight) {}
        size_t size() const {return ub-lb;}
        std::pair<SearchInterval, SearchInterval> split(float skew) const {
            assert(size() >= 2);
            if (skew <= 0 || skew >= 1) skew = 0.5;
            size_t mid = lb + (ub-lb) * skew;
            SearchInterval left {lb, mid, 0.5*weight};
            SearchInterval right {mid, ub, 0.5*weight};
            assert(left.size() > 0);
            assert(right.size() > 0);
            return {left, right};
        }
        std::pair<SearchInterval, SearchInterval> splitAt(float skew, size_t mid) const {
            double absRatio = (mid - lb) / (double)(ub - lb);
            double weightRatio = std::pow(skew, std::log(absRatio) / std::log(0.5));
            SearchInterval left {lb, mid, weightRatio * weight};
            SearchInterval right {mid, ub, (1 - weightRatio) * weight};
            return {left, right};
        }
    };

    // Invariant: Each interval in here represents an active search call, where
    // the interval's upper bound is the search call's upper bound, EXCEPT for
    // intervals in which "orphaned" is set.
    std::list<SearchInterval> _current_bounds;
    float _skew {0.9};

    std::list<size_t> _bounds_to_replay; // for debugging

public:
    IntervalSearch(float skew) : _skew(skew) {}

    void init(size_t min, size_t max) {
        // Create first interval with full weight
        _current_bounds.push_back({min, max-1, WEIGHT_SUM});
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
                if (best == _current_bounds.end() // first interval
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
            assert(left.size() > 0 && right.size() > 0);
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
    void stopTestingAndUpdateUpper(size_t bound, size_t newMax) {
        stopTestingBound(bound, false, true, newMax);
    }
    void stopTestingWithoutUpdates(size_t bound) {
        stopTestingBound(bound, false, false, 0);
    }
    void stopTestingBound(size_t bound, bool updateLower, bool updateUpper, size_t newMax) {
        if (_current_bounds.empty()) return;
        if (updateUpper) assert(newMax < bound);
        auto it = _current_bounds.begin();
        double summedWeight {0};
        while (it != _current_bounds.end()) {
            SearchInterval i = *it;
            if (updateUpper && newMax <= i.ub) {
                // the corresponding call is being stopped
                if (newMax <= i.lb) {
                    // interval is falling out of the considered range *completely*
                    it = _current_bounds.erase(it);
                } else {
                    // delete all intervals coming afterwards
                    size_t lb = it->lb;
                    while (lb < _current_bounds.back().lb) {
                        _current_bounds.pop_back();
                    }
                    // interval needs to be split so that the cutoff can be deleted
                    auto [left, right] = _current_bounds.back().splitAt(_skew, newMax);
                    // now erase the interval itself
                    _current_bounds.pop_back();
                    if (left.size() > 0) {
                        left.orphaned = i.ub == bound; // only mark as orphaned if this was your interval!
                        _current_bounds.push_back(left);
                        summedWeight += left.weight;
                        break;
                    }
                }
            } else if (updateLower && i.ub <= bound) {
                // the corresponding call stops and the interval range shrinks.
                it = _current_bounds.erase(it);
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
        for (auto i : _current_bounds) {
            out += (i.orphaned ? "~" : "")
                + std::to_string(i.weight) + "*(" + std::to_string(i.lb) + ":" + std::to_string(i.ub) + "]"
                + (i.orphaned ? "~ " : " ");
            density += std::to_string(i.weight / i.size()) + " ";
            weightTotal += i.weight;
        }
        LOG(V4_VVER, "COMBSEARCH W=%.3f %s\n", weightTotal, out.c_str());
        LOG(V5_DEBG, "COMBSEARCH DENSITY %s\n", density.c_str());
    }
};
