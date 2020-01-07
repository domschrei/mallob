
#include "permutation.h"

AdjustablePermutation::AdjustablePermutation(int n, int seed) {

    this->n = n;
    rng = std::mt19937(seed);

    // Create explicit permutations [0, sqrt(n)) -> [0, sqrt(n))
    int feistelRounds = 3;
    root_n = std::ceil(sqrt((float) n));
    feistels.reserve(feistelRounds);
    for (int f = 1; f <= feistelRounds; f++) {
        feistels.push_back(std::vector<int>());
        std::vector<int>& feistel = feistels.back();

        // Generate and shuffle number sequence
        feistel.reserve(root_n);
        for (int i = 0; i < root_n; i++) feistel.push_back(i);
        std::shuffle(std::begin(feistel), std::end(feistel), rng);
    }
};

int AdjustablePermutation::get(int x) const {

    if (adjustedValues.count(x)) {
        return adjustedValues.at(x);
    }

    int k = 0;

    do {
        // Conversion
        // x = b · n + a
        int b = x / root_n;
        int a = x % root_n;

        // Feistel rounds
        for (unsigned int i = 0; i < feistels.size(); i++) {
            int out_a = b;
            int out_b = (a + feistels[i][b]) % root_n;
            a = out_a;
            b = out_b;
        }

        // Back-conversion
        x = a + root_n * b;

        k++;

    } while (x >= n);

    return x;
}

void AdjustablePermutation::adjust(int x, int new_x) {
    if (get(x) != new_x)
        adjustedValues[x] = new_x;
}

void AdjustablePermutation::clear() {
    adjustedValues.clear();
}
