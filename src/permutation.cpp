
#include "permutation.h"

AdjustablePermutation::AdjustablePermutation(int n, int seed) {

    this->n = n;
    rng = std::mt19937(seed);

    root_n = std::ceil(sqrt((float) n));

    for (int f = 1; f <= 4; f++) {
        std::vector<int> feistel;
        for (int i = 0; i < root_n; i++) feistel.push_back(i);
        std::shuffle(std::begin(feistel), std::end(feistel), rng);
        feistels.push_back(feistel);
    }

    for (int x = 0; x < n; x++) get(x);
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
        if (k == 50) exit(1);

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
