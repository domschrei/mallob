
#pragma once

#include "app/sat/proof/lrat_op.hpp"
#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "util/random.hpp"
#include <cstring>

class LratOpTamperer {

private:
    Logger& _logger;

public:
    LratOpTamperer(Logger& logger) : _logger(logger) {}

    void tamper(LratOp& op) {
        std::vector<std::function<void(LratOp&)>> manipulators;
        if (op.isDerivation()) {
            manipulators.push_back([&](LratOp& op) {manipulateRandomLiteral(op);});
            manipulators.push_back([&](LratOp& op) {manipulateRandomHint(op);});
            manipulators.push_back([&](LratOp& op) {injectRandomLiteral(op);});
            manipulators.push_back([&](LratOp& op) {injectRandomHint(op);});
            if (op.getNbLits() > 0) manipulators.push_back([&](LratOp& op) {dropRandomLiteral(op);});
            if (op.getNbHints() > 0) manipulators.push_back([&](LratOp& op) {dropRandomNonFinalHint(op);});
            if (op.getNbHints() > 0) manipulators.push_back([&](LratOp& op) {dropFinalHint(op);});
        }
        if (op.isImport()) {
            manipulators.push_back([&](LratOp& op) {manipulateRandomImportLiteral(op);});
            manipulators.push_back([&](LratOp& op) {manipulateImportSignature(op);});
        }
        Random::choice(manipulators)(op);
    }

private:
    void dropRandomLiteral(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: dropping random lit\n", op.getId());
        int& lit = Random::choice(op.getLits(), op.getNbLits());
        if (lit != op.getLits()[op.getNbLits()-1])
            std::swap(lit, op.getLits()[op.getNbLits()-1]);
        op = LratOp(op.getId(), op.getLits(), op.getNbLits()-1, op.getHints(), op.getNbHints(), op.getGlue());
    }
    void dropRandomNonFinalHint(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: dropping random non-final hint, swapping final hint\n", op.getId());
        u64& hint = Random::choice(op.getHints(), op.getNbHints()-1);
        std::swap(hint, op.getHints()[op.getNbHints()-1]);
        op = LratOp(op.getId(), op.getLits(), op.getNbLits(), op.getHints(), op.getNbHints()-1, op.getGlue());
    }
    void dropFinalHint(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: dropping final hint\n", op.getId());
        op = LratOp(op.getId(), op.getLits(), op.getNbLits(), op.getHints(), op.getNbHints()-1, op.getGlue());
    }

    void injectRandomLiteral(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: injecting random lit\n", op.getId());
        int lits[op.getNbLits()+1];
        memcpy(lits, op.getLits(), op.getNbLits()*sizeof(int));
        lits[op.getNbLits()] = 1;
        int& swap = Random::choice(lits, op.getNbLits());
        std::swap(swap, lits[op.getNbLits()]);
        op = LratOp(op.getId(), lits, op.getNbLits()+1, op.getHints(), op.getNbHints(), op.getGlue());
    }
    void injectRandomHint(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: injecting random hint\n", op.getId());
        u64 hints[op.getNbHints()+1];
        memcpy(hints, op.getHints(), op.getNbHints()*sizeof(u64));
        hints[op.getNbHints()] = 1;
        u64& swap = Random::choice(hints, op.getNbHints());
        std::swap(swap, hints[op.getNbHints()]);
        op = LratOp(op.getId(), op.getLits(), op.getNbLits(), hints, op.getNbHints()+1, op.getGlue());
    }

    void manipulateRandomLiteral(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: manipulating random lit\n", op.getId());
        int& lit = Random::choice(op.getLits(), op.getNbLits());
        int mask = 1 << (int) (Random::rand() * 16);
        lit ^= mask;
    }
    void manipulateRandomHint(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: manipulating random hint\n", op.getId());
        u64& hint = Random::choice(op.getHints(), op.getNbHints());
        u64 mask = 1 << (int) (Random::rand() * 32);
        hint ^= mask;
    }

    void manipulateRandomImportLiteral(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with import %lu: manipulating random lit\n", op.getId());
        int& lit = Random::choice(op.getLits(), op.getNbLits());
        int mask = 1 << (int) (Random::rand() * 16);
        lit ^= mask;
    }
    void manipulateImportSignature(LratOp& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with import %lu: flipping random signature bit\n", op.getId());
        u8* sig = op.signature();
        u8 pos = (u8) (8 * SIG_SIZE_BYTES * Random::rand());
        sig[pos / 8] ^= 1 << (pos % 8);
    }
};
