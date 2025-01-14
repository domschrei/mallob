
#include "app/sat/data/model_string_compressor.hpp"
#include "util/assert.hpp"
#include "util/random.hpp"
#include "util/string_utils.hpp"
#include <climits>

void doTest(const std::vector<int>& model) {
    printf("%s\n", StringUtils::getSummary(model, 100).c_str());
    std::string compressed = ModelStringCompressor::compress(model);
    printf("%s\n", compressed.c_str());
    std::vector<int> decompressed = ModelStringCompressor::decompress(compressed);
    printf("%s\n", StringUtils::getSummary(decompressed, 100).c_str());
    assert(model == decompressed);
}

void testSimple() {
    std::vector<int> model {0, 1, -2, -3, 4, 5, 6, 7, -8, 9};
    doTest(model);
}

void testLargeRandom() {
    for (size_t size : {100'000, 100'001, 100'002, 100'003}) {
        std::vector<int> model;
        for (size_t i = 0; i <= size; i++) {
            model.push_back(i * (Random::rand() < 0.5 ? 1 : -1));
        }
        doTest(model);
    }
}

int main() {
    Random::init(0, 0);
    testSimple();
    testLargeRandom();
}
