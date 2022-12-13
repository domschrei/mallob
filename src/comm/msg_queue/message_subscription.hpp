
#pragma once

#include "comm/mympi.hpp"
#include "message_queue.hpp"

// RAII structure for a subscription to incoming messages of a certain tag.
// Subscriptions are non-exclusive, i.e., there can be multiple subscriptions
// for a given tag at the same time. Whenever a message of the concerned tag
// arrives, the supplied callback is called for each active subscription.
// The subscription is cleared upon destruction of the MessageSubscription 
// object.
class MessageSubscription {

private:
    int _tag {-1};
    MessageQueue::CallbackRef _cb_ref;

public:
    // @param tag the message tag to listen to (see msgtags.h)
    // @param callback the function to call whenever a message with the
    // supplied tag arrives
    MessageSubscription(int tag, MessageQueue::MsgCallback callback) : _tag(tag) {
        _cb_ref = MyMpi::getMessageQueue().registerCallback(tag, callback);
    }
    
    // @param tag the message tag to listen to (see msgtags.h)
    // @param callback the function to call whenever a message with the
    // supplied tag arrives
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
