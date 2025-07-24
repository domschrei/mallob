
#include "cardinality_encoding.hpp"

void cardinality_encoding_add_literal(int lit, void* instance) {
    ((CardinalityEncoding*) instance)->addLiteral(lit);
}
void cardinality_encoding_add_assumption(int lit, void* instance) {
    ((CardinalityEncoding*) instance)->addAssumption(lit);
}
