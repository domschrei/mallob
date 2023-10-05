
#include "app/sat/parse/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "data/job_description.hpp"

class QbfReader : public SatReader {

private:
    bool _reading_quantifications {false};
    bool _terminated_quantifications {false};
    enum QuantificationMode {FORALL, EXISTS} _quantification_mode;
    int _nb_added_lits {0};

public:
    QbfReader(const Parameters& params, const std::string& filename) :
        SatReader(params, filename) {}

    int getMaxVar() const { return _max_var; }

    inline void process(char c, JobDescription& desc) override {

        LOG(V6_DEBGV, "PROCESS %c\n", c);

        if (_comment && c != '\n') return;

        signed char uc = *((signed char*) &c);
        switch (uc) {
        case EOF:
            LOG(V6_DEBGV, "EOF\n");
            _input_finished = true;
        case '\n':
        case '\r':
            _comment = false;
            if (_began_num) {
                if (_num != 0) {
                    _input_invalid = true;
                    return;
                }
                finishQuantificationBlockOrClause(desc);
            }
            _reading_quantifications = false;
            break;
        case 'p':
        case 'c':
            _comment = true;
            break;
        case 'a':
            // Forall quantification
            _reading_quantifications = true;
            _quantification_mode = FORALL;
            break;
        case 'e':
            // Exists quantification
            _reading_quantifications = true;
            _quantification_mode = EXISTS;
            break;
        case ' ':
            if (_began_num) {
                _max_var = std::max(_max_var, _num);
                int lit = _sign * _num;
                if (lit == 0) {
                    finishQuantificationBlockOrClause(desc);
                } else {
                    appendNonzeroNumber(lit, desc);
                }
            }
            _sign = 1;
            break;
        case '-':
            _sign = -1;
            _began_num = true;
            break;
        default:
            // Add digit to current number
            _num = _num*10 + (c-'0');
            _began_num = true;
            break;
        }
    }

    void appendNonzeroNumber(int lit, JobDescription& desc) {
        assert(lit != 0);
        if (_reading_quantifications) {
            assert(lit > 0); // must be a variable, not a literal!
            assert(!_terminated_quantifications);
            addData((_quantification_mode == FORALL ? -1 : 1) * lit, desc);
        } else {
            if (!_terminated_quantifications) {
                // End block of quantifications.
                addData(0, desc);
                // REPEAT the block of quantifications, since in the QbfJob
                // we need a global and a local list of quantifiers.
                auto data = desc.getFormulaPayload(desc.getRevision());
                size_t prevSize = _nb_added_lits;
                for (size_t i = 0; i < prevSize; i++) {
                    addData(data[i], desc);
                }
                assert(_nb_added_lits == 2 * prevSize);
                _terminated_quantifications = true;
            }
            addData(lit, desc);
        }
    }

    void finishQuantificationBlockOrClause(JobDescription& desc) {
        LOG(V6_DEBGV, "END BLOCK\n");
        if (!_reading_quantifications) {
            addData(0, desc);
            _num_read_clauses++;
        }
        _num = 0;
        _began_num = false;
    }

    void addData(int lit, JobDescription& desc) {
        LOG(V6_DEBGV, "ADD %i\n", lit);
        desc.addPermanentData(lit);
        _nb_added_lits++;
        if (lit == 0) {
            if (!_reading_quantifications && _terminated_quantifications && _last_added_lit_was_zero)
                _contains_empty_clause = true;
            _last_added_lit_was_zero = true;
        } else _last_added_lit_was_zero = false;
        _num = 0;
        _began_num = false;
    }
};
