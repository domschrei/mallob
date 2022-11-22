
#pragma once

#include "mympi.hpp"
#include "message_queue.hpp"

class MessageSubscription {

private:
    int _tag {-1};
    MessageQueue::CallbackRef _cb_ref;

public:
    MessageSubscription(int tag, MessageQueue::MsgCallback callback) : _tag(tag) {
        _cb_ref = MyMpi::getMessageQueue().registerCallback(tag, callback);
    }
    
    MessageSubscription(int tag, void (*callback)(MessageHandle&)) : _tag(tag) {
        _cb_ref = MyMpi::getMessageQueue().registerCallback(tag, [&, callback](auto& h) {
            callback(h);
        });
    }
    
    MessageSubscription(MessageSubscription&& moved) {
        _tag = moved._tag;
        _cb_ref = std::move(moved._cb_ref);
        moved._tag = -1;
    }

    MessageSubscription& operator=(MessageSubscription&& moved) {
        _tag = moved._tag;
        _cb_ref = std::move(moved._cb_ref);
        moved._tag = -1;
        return *this;
    }

    ~MessageSubscription() {
        if (_tag != -1)
            MyMpi::getMessageQueue().clearCallback(_tag, _cb_ref);
    }
};
