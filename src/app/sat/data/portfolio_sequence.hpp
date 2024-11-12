
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

struct PortfolioSequence {
    enum BaseSolver {
        KISSAT = 'k',
        CADICAL = 'c',
        LINGELING = 'l',
        GLUCOSE = 'g',
        MERGESAT = 'm',
        VARIABLE_ADDITION = 'v'
    };
    enum Flavour {
        DEFAULT, SAT, UNSAT
    };
    struct Item {
        BaseSolver baseSolver;
        Flavour flavour {DEFAULT};
        bool incremental {false};
        bool outputProof {false};
    };

    std::vector<Item> prefix;
    std::vector<Item> cycle;

    bool parse(const std::string& descriptor) {
        prefix.clear();
        cycle.clear();
        if (!parse(descriptor, 1, prefix, cycle)) return false;
        // no explicit cycle provided => everything is the cycle
        if (cycle.empty()) {
            cycle = prefix;
            prefix.clear();
        }
        return true;
    }

    std::string toStr() const {
        std::string out = "[ ";
        for (auto& item : prefix) out += std::to_string(item.baseSolver) + "/" + std::to_string(item.flavour) + "/" + std::to_string(item.incremental) + "/" + std::to_string(item.outputProof) + " ";
        out += "] ( ";
        for (auto& item : cycle) out += std::to_string(item.baseSolver) + "/" + std::to_string(item.flavour) + "/" + std::to_string(item.incremental) + "/" + std::to_string(item.outputProof) + " ";
        out += ")*";
        return out;
    }

    bool featuresProofOutput() const {
        return std::any_of(prefix.begin(), prefix.end(), [&](auto& i) {return i.outputProof;})
            || std::any_of(cycle.begin(), cycle.end(), [&](auto& i) {return i.outputProof;});
    }

private:

    bool parse(const std::string& descriptor, int nbRepetitions, std::vector<Item>& prefix, std::vector<Item>& cycle) {

        //printf("PARSE \"%s\"\n", descriptor.c_str());

        size_t i = 0;
        Item next;
        bool begun = false;
        while (i < descriptor.size()) {
            char c = descriptor[i];
            bool newSolver = std::isalpha(c);
            if (newSolver) {
                if (begun) {
                    prefix.push_back(next);
                    next = Item();
                }
                begun = true;
            }
            next.incremental = std::isupper(c);
            c = std::tolower(c);
            switch (c) {
            case 'k':
                next.baseSolver = KISSAT;
                break;
            case 'c':
                next.baseSolver = CADICAL;
                break;
            case 'l':
                next.baseSolver = LINGELING;
                break;
            case 'g':
                next.baseSolver = GLUCOSE;
                break;
            case 'm':
                next.baseSolver = MERGESAT;
                break;
            case 'v':
                next.baseSolver = VARIABLE_ADDITION;
                break;
            case '(': {
                if (begun) {
                    prefix.push_back(next);
                    next = Item();
                    begun = false;
                }
                // scan # solvers to instantiate, expand
                i++;
                const auto start = i; // position after opening "("
                int nbOpen = 1;
                while (i < descriptor.size() && nbOpen > 0) {
                    if (descriptor[i] == '(') nbOpen++;
                    if (descriptor[i] == ')') nbOpen--;
                    i++;
                }
                const auto end = i-1; // position of closing ")"
                if (nbOpen > 0) return false;
                int nbReps = 1;
                bool isCycle = false;
                if (i < descriptor.size() && descriptor[i] == '{') {
                    i++;
                    nbReps = 0;
                    while (i < descriptor.size() && descriptor[i] != '}') {
                        if (!std::isdigit(descriptor[i])) return false;
                        nbReps = nbReps*10 + (descriptor[i]-'0');
                        i++;
                    }
                    //printf("%i\n", nbReps);
                } else if (i < descriptor.size() && descriptor[i] == '*') {
                    if (!cycle.empty()) return false;
                    isCycle = true;
                } else return false;
                std::vector<Item> subPrefix, subCycle;
                if (!parse(descriptor.substr(start, end-start), nbReps,
                    subPrefix, subCycle)) return false;
                if (!subCycle.empty()) return false;
                if (isCycle) {
                    cycle.insert(cycle.end(), subPrefix.begin(), subPrefix.end());
                } else {
                    prefix.insert(prefix.end(), subPrefix.begin(), subPrefix.end());
                }
                break; }
            case '+':
                // SAT flavour
                next.flavour = SAT;
                break;
            case '-':
                // UNSAT flavour
                next.flavour = UNSAT;
                break;
            case '!':
                // Proof generation
                next.outputProof = true;
                break;
            case '*':
                // Last item is the cycle
                if (!begun) return false;
                if (!cycle.empty()) return false;
                cycle.push_back(next);
                next = Item();
                begun = false;
                break;
            default:
                return false;
            }
            i++;
        }
        if (begun) {
            prefix.push_back(next);
        }

        //printf("BEFORE %lu %lu\n", prefix.size(), cycle.size());

        // Apply repetitions
        const size_t sizeBeforePrefix = prefix.size();
        const size_t sizeBeforeCycle = cycle.size();
        while (nbRepetitions > 1) {
            for (size_t i = 0; i < sizeBeforePrefix; i++) prefix.push_back(prefix[i]);
            for (size_t i = 0; i < sizeBeforeCycle; i++) cycle.push_back(cycle[i]);
            nbRepetitions--;
        }
        //printf("AFTER %lu %lu\n", prefix.size(), cycle.size());
        return true;
    }
};
