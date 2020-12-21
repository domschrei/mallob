
#include "message_handler.hpp" 

const int MessageHandler::TAG_DEFAULT = -42;

MessageHandler::MessageHandler() {

}

void MessageHandler::registerCallback(int tag, const MsgCallback& cb) {
    _callbacks[tag] = cb;
}

void MessageHandler::pollMessages(float elapsedTime) {
    // Process new messages
    auto handle = MyMpi::poll(elapsedTime);
    if (handle) {
        if (_callbacks.count(handle->tag)) _callbacks[handle->tag](handle);
        else if (_callbacks.count(TAG_DEFAULT)) _callbacks[TAG_DEFAULT](handle);
    }
}