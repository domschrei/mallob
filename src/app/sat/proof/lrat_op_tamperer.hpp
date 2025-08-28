
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
            auto& data = op.data.produce;
            manipulators.push_back([&](LratOp& op) {manipulateRandomLiteral(data);});
            manipulators.push_back([&](LratOp& op) {manipulateRandomHint(data);});
            //manipulators.push_back([&](LratOp& op) {injectRandomLiteral(data);});
            //manipulators.push_back([&](LratOp& op) {injectRandomHint(data);});
            if (data.nbLits > 0) manipulators.push_back([&](LratOp& op) {dropRandomLiteral(data);});
            if (data.nbHints > 0) manipulators.push_back([&](LratOp& op) {dropRandomNonFinalHint(data);});
            if (data.nbHints > 0) manipulators.push_back([&](LratOp& op) {dropFinalHint(data);});
        }
        if (op.isImport()) {
            manipulators.push_back([&](LratOp& op) {manipulateRandomImportLiteral(op.data.import);});
            manipulators.push_back([&](LratOp& op) {manipulateImportSignature(op.data.import);});
        }
        Random::choice(manipulators)(op);
    }

private:
    void dropRandomLiteral(LratOp::LratOpData::LratOpDataProduce& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: dropping random lit\n", op.id);
        int& lit = Random::choice(op.lits, op.nbLits);
        if (lit != op.lits[op.nbLits-1])
            std::swap(lit, op.lits[op.nbLits-1]);
        op.nbLits--;
    }
    void dropRandomNonFinalHint(LratOp::LratOpData::LratOpDataProduce& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: dropping random non-final hint, swapping final hint\n", op.id);
        u64& hint = Random::choice(op.hints, op.nbHints-1);
        std::swap(hint, op.hints[op.nbHints-1]);
        op.nbHints--;
    }
    void dropFinalHint(LratOp::LratOpData::LratOpDataProduce& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: dropping final hint\n", op.id);
        op.nbHints--;
    }

    void manipulateRandomLiteral(LratOp::LratOpData::LratOpDataProduce& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: manipulating random lit\n", op.id);
        int& lit = Random::choice(op.lits, op.nbLits);
        int mask = 1 << (int) (Random::rand() * 16);
        lit ^= mask;
    }
    void manipulateRandomHint(LratOp::LratOpData::LratOpDataProduce& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with derivation %lu: manipulating random hint\n", op.id);
        u64& hint = Random::choice(op.hints, op.nbHints);
        u64 mask = 1 << (int) (Random::rand() * 32);
        hint ^= mask;
    }

    void manipulateRandomImportLiteral(LratOp::LratOpData::LratOpDataImport& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with import %lu: manipulating random lit\n", op.id);
        int& lit = Random::choice(op.lits, op.nbLits);
        int mask = 1 << (int) (Random::rand() * 16);
        lit ^= mask;
    }
    void manipulateImportSignature(LratOp::LratOpData::LratOpDataImport& op) {
        LOGGER(_logger, V2_INFO, "TAMPERING with import %lu: flipping random signature bit\n", op.id);
        u8* sig = op.sig;
        u8 pos = (u8) (8 * SIG_SIZE_BYTES * Random::rand());
        sig[pos / 8] ^= 1 << (pos % 8);
    }
};
