
#include "formula_compressor.hpp"

int compare_compressed_lits(const void* a, const void* b) {
    unsigned int compr_a = (* (unsigned int*) a);
    unsigned int compr_b = (* (unsigned int*) b);
    return compr_a - compr_b;
}
