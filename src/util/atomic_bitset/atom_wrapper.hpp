
#ifndef DOMPASCH_MALLOB_ATOM_WRAPPER_HPP
#define DOMPASCH_MALLOB_ATOM_WRAPPER_HPP

/*
 * A wrapper that allows us to construct a vector of atomic elements
 * https://stackoverflow.com/questions/13193484/how-to-declare-a-vector-of-atomic-in-c
 */

#include <atomic>

template <typename T>
struct atomwrapper {
    std::atomic<T> _a;
    atomwrapper() : _a() { }
    atomwrapper(T desired) : _a(desired) {}
    atomwrapper(const std::atomic<T> &a) : _a(a.load()) { }
    atomwrapper(const atomwrapper &other) : _a(other._a.load()) { }
    inline atomwrapper &operator=(const atomwrapper &other) {
        _a.store(other._a.load());
        return *this;
    }
    inline std::atomic<T>& ref(void) { return _a; }
    inline const std::atomic<T>& const_ref(void) const { return _a; }
};

#endif
