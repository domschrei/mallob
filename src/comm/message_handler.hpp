
#ifndef DOMPASCH_MALLOB_MESSAGE_HANDLER
#define DOMPASCH_MALLOB_MESSAGE_HANDLER

#include "util/hashing.hpp"
#include <functional>

#include "mympi.hpp"

typedef std::function<void(MessageHandle&)> MsgCallback;

class MessageHandler {

public:
    static const int TAG_DEFAULT;

private:
    robin_hood::unordered_map<int, MsgCallback> _callbacks;

public:
    MessageHandler();
    void registerCallback(int tag, const MsgCallback& cb);
    void pollMessages(float elapsedTime);

};

#endif