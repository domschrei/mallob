
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

    int input = x;
    int k = 0;

    while (true) {
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

        // Prerequisite 1: must be in valid domain [0, n)
        if (x >= _n) continue;
        // Prerequisite 2: must not map to identity
        if (_identity_disallowed && x == input) continue;
        // Prerequisite 3: must not map to the according value 
        // of some of the disallowed permutations
        bool accept = true;
        for (const auto& p : _disallowed_permutations) {
            if (x == p->get(input)) {
                accept = false;
                break;
            }
        }
        if (accept) break;
    }

    return x;
}

void AdjustablePermutation::adjust(int x, int new_x) {
    if (get(x) != new_x)
        _adjusted_values[x] = new_x;
}

void AdjustablePermutation::clear() {
    _adjusted_values.clear();
}

void AdjustablePermutation::setIdentityDisallowed(const bool& disallow) {
    _identity_disallowed = disallow;
}

void AdjustablePermutation::addDisallowedPermutation(AdjustablePermutation* p) {
    _disallowed_permutations.push_back(p);
}