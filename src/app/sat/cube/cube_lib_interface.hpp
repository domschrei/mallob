#ifndef MSCHICK_CUBE_LIB_INTERFACE_H
#define MSCHICK_CUBE_LIB_INTERFACE_H

#include <atomic>

#include "cube_communicator.hpp"
#include "cube_setup.hpp"

class CubeLibInterface {
   protected:
    // Flag that blocks all communication on interruption
    std::atomic_bool _isInterrupted{false};

    virtual bool wantsToCommunicateImpl() = 0;
    virtual void beginCommunicationImpl() = 0;
    virtual void handleMessageImpl(int source, JobMessage &msg) = 0;

   public:
    CubeLibInterface();

    virtual ~CubeLibInterface();

    bool wantsToCommunicate() {
        if (!_isInterrupted)
            wantsToCommunicateImpl();
        else
            return false;
    };
    void beginCommunication() {
        if (!_isInterrupted)
            beginCommunicationImpl();
    };
    void handleMessage(int source, JobMessage &msg) {
        if (!_isInterrupted)
            handleMessageImpl(source, msg);
    };

    virtual void startWorking() = 0;

    // Makes worker thread terminate asynchronously
    // Requires that startWorking was called previously
    // Disables all communication methods
    virtual void interrupt() = 0;

    // Joins all started
    // Requires that interrupt was called previously
    virtual void withdraw() = 0;

    // Suspends all working threads
    virtual void suspend() = 0;

    // Resumes all working threads
    virtual void resume() = 0;
};

#endif /* MSCHICK_CUBE_LIB_INTERFACE_H */