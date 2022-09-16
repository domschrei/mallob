
#include "lrat_utils.hpp"
#include "util/assert.hpp"


namespace lrat_utils {

    void writeLine(WriteBuffer& buf, const LratLine& line) {
        buf.writeLineHeader();
        int64_t signedId = line.id;
        buf.writeSignedClauseId(signedId, WriteMode::NORMAL);
        for (int lit : line.literals) {
            buf.writeLiteral(lit, WriteMode::NORMAL);
        }
        buf.writeSeparator();
        for (size_t i = 0; i < line.hints.size(); i++) {
            int64_t signedId = (line.signsOfHints[i] ? 1 : -1) * line.hints[i];
            buf.writeSignedClauseId(signedId, WriteMode::NORMAL);
        }
        buf.writeSeparator();
    }

    void writeLine(WriteBuffer& buf, SerializedLratLine& line, WriteMode mode) {
        if (mode == REVERSED) {
            buf.writeSeparator();
            auto [hints, numHints] = line.getUnsignedHints();
            auto signs = line.getSignsOfHints();
            for (int i = numHints-1; i >= 0; i--) {
                int64_t signedId = (signs[i] ? 1 : -1) * hints[i];
                buf.writeSignedClauseId(signedId, mode);
            }
            buf.writeSeparator();
            auto [lits, size] = line.getLiterals();
            for (int i = size-1; i >= 0; i--) {
                buf.writeLiteral(lits[i], mode);
            }
            int64_t signedId = line.getId();
            buf.writeSignedClauseId(signedId, mode);
            buf.writeLineHeader();
        } else {
            buf.writeLineHeader();
            int64_t signedId = line.getId();
            buf.writeSignedClauseId(signedId, mode);
            auto [lits, size] = line.getLiterals();
            for (size_t i = 0; i < size; i++) {
                buf.writeLiteral(lits[i], mode);
            }
            buf.writeSeparator();
            auto [hints, numHints] = line.getUnsignedHints();
            auto signs = line.getSignsOfHints();
            for (size_t i = 0; i < numHints; i++) {
                int64_t signedId = (signs[i] ? 1 : -1) * hints[i];
                buf.writeSignedClauseId(signedId, mode);
            }
            buf.writeSeparator();
        }
    }

    void writeDeletionLine(WriteBuffer& buf, LratClauseId headerId, 
            const std::vector<unsigned long>& ids, WriteMode mode) {
        
        writeDeletionLine(buf, headerId, ids.data(), ids.size(), mode);
    }

    void writeDeletionLine(WriteBuffer& buf, LratClauseId headerId, 
        const unsigned long* ids, int numHints, WriteMode mode) {

        if (mode == REVERSED) {
            buf.writeSeparator();
            for (int i = numHints-1; i >= 0; i--) {
                buf.writeSignedClauseId(ids[i], mode);
            }
            buf.writeDeletionLineHeader();
        } else {
            buf.writeDeletionLineHeader();
            for (int i = 0; i < numHints; i++) {
                auto id = ids[i];
                buf.writeSignedClauseId(id, mode);
            }
            buf.writeSeparator();
        }
    }

    bool readLine(ReadBuffer& buf, LratLine& line) {
        
        if (buf.endOfFile()) return false;

        line.id = -1;
        line.literals.clear();
        line.hints.clear();
        line.signsOfHints.clear();

        int header = buf.get();
        if (header != 'a') return false;

        int64_t signedId;
        if (!buf.readSignedClauseId(signedId)) return false;
        assert(signedId > 0);
        line.id = signedId;

        int lit;
        while (buf.readLiteral(lit)) {
            line.literals.push_back(lit);
        }
        // separator zero was read by "readLiteral" call that returned zero

        while (buf.readSignedClauseId(signedId)) {
            line.hints.push_back(std::abs(signedId));
            line.signsOfHints.push_back(signedId>0);
        }
        // line termination zero was read by "readLiteral" call that returned zero

        return true;
    }

    /*
    template <typename T>
    void backInsert(std::vector<uint8_t>& data, const T& thing) {
        auto pos = data.size();
        data.resize(pos + sizeof(T));
        memcpy(data.data()+pos, &thing, sizeof(T));
    }
    */
    
    template <typename T>
    void backInsert(std::vector<uint8_t>& data, const T& thing) {
        data.insert(data.end(), (uint8_t*) &thing, ((uint8_t*) (&thing))+sizeof(T));
    }

    bool readLine(ReadBuffer& buf, SerializedLratLine& line) {

        if (buf.endOfFile()) return false;

        int header = buf.get();
        if (header != 'a') return false;

        // LratClauseId id;
        // int numLiterals;
        // int literals[numLiterals];
        // int numHints;
        // LratClauseId hints[numHints];
        // bool signsOfHints[numHints];
        std::vector<uint8_t>& data = line.data();
        data.clear();

        int64_t signedId;
        if (!buf.readSignedClauseId(signedId)) return false;
        assert(signedId > 0);
        backInsert(data, (unsigned long) signedId);

        // literals counter
        int litCounterPos = data.size();
        backInsert(data, (int)0);

        // actual literals
        int numLiterals = 0;
        int lit;
        while (buf.readLiteral(lit)) {
            backInsert(data, lit);
            numLiterals++;
        }
        // separator zero was read by "readLiteral" call that returned zero

        // update literals counter
        memcpy(data.data()+litCounterPos, &numLiterals, sizeof(int));

        // hints counter
        int hintCounterPos = data.size();
        backInsert(data, (int)0);

        // actual hints
        int numHints = 0;
        while (buf.readSignedClauseId(signedId)) {
            backInsert(data, signedId);
            numHints++;
        }
        // line termination zero was read by "readLiteral" call that returned zero

        // update hints counter
        memcpy(data.data()+hintCounterPos, &numHints, sizeof(int));

        // separate and append signs of hints from hints
        for (int i = 0; i < numHints; i++) {
            long* hint = (long*) (data.data()+hintCounterPos+sizeof(int)+i*sizeof(long));
            bool sign = *hint > 0;
            *hint = (unsigned long) std::abs(*hint);
            backInsert(data, (char) (sign ? 1 : 0));
        }
        
        return true;
    }
}
