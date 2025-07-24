
#pragma once

template <typename T>
class MergeSourceInterface {

public:
    virtual bool pollBlocking(T& elem) = 0;
    virtual size_t getCurrentSize() const = 0;
    virtual ~MergeSourceInterface() {}
};
