
#pragma once

#include <list>
#include <vector>

#include "merge_source_interface.hpp"

template <typename T>
class SmallMerger : public MergeSourceInterface<T> {

private:
    std::list<std::pair<MergeSourceInterface<T>*, T>> _sources;
    bool _initialized {false};
    
public:
    SmallMerger(std::vector<MergeSourceInterface<T>*>& sources) {
        // Initialize sources
        for (auto& source : sources) {
            _sources.emplace_back();
            _sources.back().first = source;
        }
    }

    bool pollBlocking(T& output) override {

        if (!_initialized) initialize();

        // Find next best line
        auto bestIt = _sources.end();
        T* bestElem = nullptr;

        for (auto it = _sources.begin(); it != _sources.end(); ++it) {
            auto& [source, nextElem] = *it;

            // Better than previous element?
            if (bestElem == nullptr || nextElem > *bestElem) {
                bestIt = it;
                bestElem = &nextElem;
            }
        }

        // No element found?
        if (bestElem == nullptr) return false;
        
        // Write output
        std::swap(output, *bestElem);
        
        // Poll next element for chosen merger,
        // deleting it if it has none
        auto& [source, nextElem] = *bestIt;
        if (!source->pollBlocking(nextElem)) {
            // No element left for this merger: delete
            _sources.erase(bestIt);
        }

        return true;
    }

    size_t getCurrentSize() const override {
        auto size = 0;
        for (auto& [source, nextElem] : _sources) {
            size += source->getCurrentSize();
        }
        return size;
    }

    std::string getReport() {
        std::string sizes;
        for (auto& [source, nextElem] : _sources) {
            sizes += std::to_string(source->getCurrentSize()) + " ";
        }
        return sizes;
    }

private:
     void initialize() {
        // Do polling of initial elements for each merger
        // (deleting them directly if they have none)
        auto it = _sources.begin();
        while (it != _sources.end()) {
            auto& [source, nextElem] = *it;
            if (source->pollBlocking(nextElem)) {
                // successful poll: accept merger
                ++it;
            } else {
                // no success: delete this merger
                it = _sources.erase(it);
            }
        }
        _initialized = true;
    }
};
