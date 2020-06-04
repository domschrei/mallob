
#include "message_handler.h" 

const int MessageHandler::TAG_DEFAULT = -42;

MessageHandler::MessageHandler() {

}

void MessageHandler::registerCallback(int tag, const MsgCallback& cb) {
    _callbacks[tag] = cb;
}

void MessageHandler::pollMessages(float elapsedTime) {
    std::vector<MessageHandlePtr> handles = MyMpi::poll(elapsedTime);
    // Process new messages
    for (MessageHandlePtr& handle : handles) {
        if (_callbacks.count(handle->tag)) _callbacks[handle->tag](handle);
        else if (_callbacks.count(TAG_DEFAULT)) _callbacks[TAG_DEFAULT](handle);
    }
}