
#include "permutation.h"

AdjustablePermutation::AdjustablePermutation(int n, int seed) {

    _n = n;
    _rng = std::mt19937(seed);

    // Create explicit permutations [0, sqrt(n)) -> [0, sqrt(n))
    int feistelRounds = 3;
    _root_n = std::ceil(sqrt((float) n));
    _feistels.reserve(feistelRounds);
    for (int f = 1; f <= feistelRounds; f++) {
        _feistels.push_back(std::vector<int>());
        std::vector<int>& feistel = _feistels.back();

        // Generate and shuffle number sequence
        feistel.reserve(_root_n);
        for (int i = 0; i < _root_n; i++) feistel.push_back(i);
        std::shuffle(std::begin(feistel), std::end(feistel), _rng);
    }
};

int AdjustablePermutation::get(int x) const {

    if (_adjusted_values.count(x)) {
        return _adjusted_values.at(x);
    }

    int k = 0;

    do {
        // Conversion
        // x = b · n + a
        int b = x / _root_n;
        int a = x % _root_n;

        // Feistel rounds
        for (unsigned int i = 0; i < _feistels.size(); i++) {
            int out_a = b;
            int out_b = (a + _feistels[i][b]) % _root_n;
            a = out_a;
            b = out_b;
        }

        // Back-conversion
        x = a + _root_n * b;

        k++;

    } while (x >= _n);

    return x;
}

void AdjustablePermutation::adjust(int x, int new_x) {
    if (get(x) != new_x)
        _adjusted_values[x] = new_x;
}

void AdjustablePermutation::clear() {
    _adjusted_values.clear();
}
