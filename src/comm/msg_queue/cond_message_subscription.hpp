
#pragma once

#include "comm/mympi.hpp"
#include "message_queue.hpp"

// RAII structure for a subscription to incoming messages of a certain tag.
// Subscriptions are non-exclusive, i.e., there can be multiple subscriptions
// for a given tag at the same time. Whenever a message of the concerned tag
// arrives, the supplied callback is called for each active subscription.
// The subscription is cleared upon destruction of the MessageSubscription 
// object.
class CondMessageSubscription {

private:
    int _tag {-1};
    MessageQueue::CondCallbackRef _cb_ref;

public:
    // @param tag the message tag to listen to (see msgtags.h)
    // @param callback the function to call whenever a message with the
    // supplied tag arrives
    CondMessageSubscription(int tag, MessageQueue::ConditionalMsgCallback callback) : _tag(tag) {
        _cb_ref = MyMpi::getMessageQueue().registerConditionalCallback(tag, callback);
    }
    
    // @param tag the message tag to listen to (see msgtags.h)
    // @param callback the function to call whenever a message with the
    // supplied tag arrives
    CondMessageSubscription(int tag, bool (*callback)(MessageHandle&)) : _tag(tag) {
        _cb_ref = MyMpi::getMessageQueue().registerConditionalCallback(tag, [&, callback](auto& h) {
            return callback(h);
        });
    }
    
    CondMessageSubscription(CondMessageSubscription&& moved) {
        _tag = moved._tag;
        _cb_ref = std::move(moved._cb_ref);
        moved._tag = -1;
    }
    CondMessageSubscription& operator=(CondMessageSubscription&& moved) {
        _tag = moved._tag;
        _cb_ref = std::move(moved._cb_ref);
        moved._tag = -1;
        return *this;
    }

    void reset() {
        if (_tag != -1)
            MyMpi::getMessageQueue().clearConditionalCallback(_tag, _cb_ref);
        _tag = -1;
    }

    ~CondMessageSubscription() {
        reset();
    }
};
