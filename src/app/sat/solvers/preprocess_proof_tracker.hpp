
#pragma once

#include "robin_map.h"
#include "util/hashing.hpp"
#include <fstream>

// Serves the dual purpose of writing a DRAT proof to an output file
// while keeping track of the solver's clause set and outputting its final CNF in the end.
class PreprocessProofTracker {

private:
    int _nb_vars;
    const std::string _cnf_output_path;
    const std::string _proof_output_path;
    bool _tracks_cnf;

    int _nb_clauses {0};
    struct ClauseHasher {
        size_t operator()(const std::vector<int>& vec) const {
            size_t h = 1449;
            for (int l : vec) hash_combine(h, l);
            return h;
        }
    };
    tsl::robin_map<std::vector<int>, int, ClauseHasher> _clause_map;

    std::vector<int> _orig_cls;

    std::ofstream _ofs_proof;

public:
    PreprocessProofTracker(int nbVars, const std::string& cnfOutputPath, const std::string& proofOutputPath)
        : _nb_vars(nbVars), _cnf_output_path(cnfOutputPath), _proof_output_path(proofOutputPath),
        _tracks_cnf(!_cnf_output_path.empty()) {

        _ofs_proof = std::ofstream(_proof_output_path, std::ios::binary);
    }

    void appendOriginalLiteral(int lit) {
        if (!_tracks_cnf) return;
        if (lit == 0) {
            //logProofOrigClause(_orig_cls.data(), _orig_cls.size());
            addClause(_orig_cls.data(), _orig_cls.size());
            _orig_cls.clear();
        } else {
            _orig_cls.push_back(lit);
        }
    }

    void appendDerivation(const int* lits, int nbLits) {
        logProofAddition(lits, nbLits);
        addClause(lits, nbLits);
    }

    void appendDeletion(const int* lits, int nbLits) {
        logProofDeletion(lits, nbLits);
        deleteClause(lits, nbLits);
    }

    void finalizeOutput() {
        _ofs_proof.close();
        if (!_tracks_cnf) return;
        std::ofstream ofs(_cnf_output_path);
        ofs << "p cnf " << _nb_vars << " " << _nb_clauses << std::endl;
        for (auto& [cls, occ] : _clause_map) {
            for (int lit : cls) ofs << lit << " ";
            ofs << "0" << std::endl;
        }
    }

    bool tracksCnf() const {
        return _tracks_cnf;
    }

private:

    void addClause(const int* lits, int nbLits) {
        if (!_tracks_cnf) return;
        auto vec = litsToVec(lits, nbLits);
        _clause_map[vec]++;
        _nb_clauses++;
    }
    void deleteClause(const int* lits, int nbLits) {
        if (!_tracks_cnf) return;
        auto vec = litsToVec(lits, nbLits);
        auto& item = _clause_map[vec];
        if (item == 0) {
            // ERROR: clause not found!
            abort();
        }
        item--;
        if (item == 0) {
            // delete entry
            _clause_map.erase(vec);
        }
        _nb_clauses--;
    }

    void logProofAddition(const int* lits, int nbLits) {
        print_binary_proof_line(lits, nbLits, 'a');
    }
    void logProofDeletion(const int* lits, int nbLits) {
        print_binary_proof_line(lits, nbLits, 'd');
    }
    // invalid DRAT - just for debugging
    void logProofOrigClause(const int* lits, int nbLits) {
        print_binary_proof_line(lits, nbLits, 'o');
    }

    std::vector<int> litsToVec(const int* lits, int nbLits) {
        auto vec = std::vector<int>(lits, lits+nbLits);
        std::sort(vec.begin(), vec.end());
        // Need to update #variables due to variable addition ...
        if (nbLits > 0) {
            _nb_vars = std::max(_nb_vars, (int) -vec.front());
            _nb_vars = std::max(_nb_vars, (int) vec.back());
        }
        return vec;
    }

    void write_char(unsigned char c) {
        _ofs_proof << c;
    }

    // taken from Kissat
    void print_binary_proof_line(const int* lits, int nbLits, unsigned char type) {
        write_char(type);
        for (int i = 0; i < nbLits; i++) {
            int elit = lits[i];
            unsigned x = 2u * std::abs(elit) + (elit < 0);
            unsigned char ch;
            while (x & ~0x7f) {
            ch = (x & 0x7f) | 0x80;
            write_char(ch);
            x >>= 7;
            }
            write_char(x);
        }
        write_char(0);
    }
};
