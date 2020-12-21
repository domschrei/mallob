
#include "message_handler.hpp" 

const int MessageHandler::TAG_DEFAULT = -42;

MessageHandler::MessageHandler() {

}

void MessageHandler::registerCallback(int tag, const MsgCallback& cb) {
    _callbacks[tag] = cb;
}

void MessageHandler::pollMessages(float elapsedTime) {
    // Process new messages
    auto maybeHandle = MyMpi::poll(elapsedTime);
    if (maybeHandle) {
        auto& handle = maybeHandle.value();
        Console::log_recv(Console::VVVERB, handle.source, "Handle Msg ID=%i tag=%i", handle.id, handle.tag);
        if (_callbacks.count(handle.tag)) _callbacks[handle.tag](handle);
        else if (_callbacks.count(TAG_DEFAULT)) _callbacks[TAG_DEFAULT](handle);
    }
}