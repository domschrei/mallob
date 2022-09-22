
#pragma once

#include <list>
#include <vector>

#include "merge_source_interface.hpp"

template <typename T>
class SmallMerger : public MergeSourceInterface<T> {

private:
    struct Source {
        MergeSourceInterface<T>* interface;
        T nextElem;
        bool valid {false};
    };
    std::list<Source> _sources;
    
public:
    SmallMerger(std::vector<MergeSourceInterface<T>*>& sources) {
        // Initialize sources
        for (auto& source : sources) {
            _sources.emplace_back();
            _sources.back().interface = source;
        }
    }

    bool pollBlocking(T& output) override {

        // Find next best line
        Source* bestSource = nullptr;        
        for (auto it = _sources.begin(); it != _sources.end(); ) {
            auto& source = *it;

            if (!source.valid && !source.interface->pollBlocking(source.nextElem)) {
                // No element left for this merger: delete
                it = _sources.erase(it);
                continue;
            }
            source.valid = true;

            // Better than previous element? -> Remember
            if (bestSource == nullptr || source.nextElem > bestSource->nextElem) {
                bestSource = &source;
            }

            ++it;
        }

        // No element found?
        if (bestSource == nullptr) return false;
        
        // Write output, mark source as no longer valid
        std::swap(output, bestSource->nextElem);
        bestSource->valid = false;
        
        return true;
    }

    size_t getCurrentSize() const override {
        auto size = 0;
        for (auto& source : _sources) {
            size += source.interface->getCurrentSize()+(source.valid ? 1 : 0);
        }
        return size;
    }

    std::string getReport() {
        std::string sizes;
        for (auto& source : _sources) {
            sizes += std::to_string(
                source.interface->getCurrentSize()+(source.valid ? 1 : 0)
            ) + " ";
        }
        return sizes;
    }
};
