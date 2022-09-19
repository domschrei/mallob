
#pragma once

template <typename T>
class MergeSourceInterface {

public:
    virtual bool pollBlocking(T& elem) = 0;

};
