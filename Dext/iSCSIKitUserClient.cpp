// User client bridging iscsikitd to the virtual HBA.

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSData.h>
#include "iSCSIKitDext.h"
#include "iSCSIKitUserClient.h"
#include "iSCSIKitProtocol.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "iSCSIKitUserClient: " fmt, ##__VA_ARGS__)

struct iSCSIKitUserClient_IVars {
    iSCSIKitDext * controller;      // provider, not retained
    OSAction * pendingCallback;     // retained
};

bool iSCSIKitUserClient::init()
{
    if (!super::init()) {
        return false;
    }
    ivars = IONewZero(iSCSIKitUserClient_IVars, 1);
    return ivars != nullptr;
}

void iSCSIKitUserClient::free()
{
    if (ivars) {
        OSSafeReleaseNULL(ivars->pendingCallback);
        IOSafeDeleteNULL(ivars, iSCSIKitUserClient_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(iSCSIKitUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // ExternalMethod carries QUEUENAME(IOUserClientQueueExternalMethod): give
    // it a dedicated serial queue. Without it, external methods dispatch on
    // the controller's Default queue and any synchronous framework call that
    // needs kernel->dext callbacks (UserCreateTargetForID, probe I/O)
    // deadlocks the whole stack.
    IODispatchQueue * methodQueue = nullptr;
    ret = IODispatchQueue::Create("IOUserClientQueueExternalMethod", 0, 0, &methodQueue);
    if (ret != kIOReturnSuccess || !methodQueue) {
        LOG("failed to create external-method queue: 0x%x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ret = SetDispatchQueue("IOUserClientQueueExternalMethod", methodQueue);
    methodQueue->release();
    if (ret != kIOReturnSuccess) {
        LOG("failed to set external-method queue: 0x%x", ret);
        return ret;
    }
    ivars->controller = OSDynamicCast(iSCSIKitDext, provider);
    if (!ivars->controller) {
        return kIOReturnBadArgument;
    }
    ivars->controller->DaemonSetUserClient(this);
    LOG("daemon connected");
    return kIOReturnSuccess;
}

kern_return_t IMPL(iSCSIKitUserClient, Stop)
{
    LOG("daemon disconnected");
    if (ivars->controller) {
        ivars->controller->DaemonSetUserClient(nullptr);
        ivars->controller = nullptr;
    }
    OSSafeReleaseNULL(ivars->pendingCallback);
    return Stop(provider, SUPERDISPATCH);
}

void iSCSIKitUserClient::NotifyTaskPending(uint64_t taskID)
{
    if (!ivars->pendingCallback) {
        LOG("task %llu pending but no callback registered", taskID);
        return;
    }
    uint64_t asyncData[1] = { taskID };
    AsyncCompletion(ivars->pendingCallback, kIOReturnSuccess, asyncData, 1);
}

kern_return_t iSCSIKitUserClient::ExternalMethod(uint64_t selector,
                                                 IOUserClientMethodArguments * arguments,
                                                 const IOUserClientMethodDispatch * dispatch,
                                                 OSObject * target,
                                                 void * reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;

    if (!ivars->controller) {
        return kIOReturnNotReady;
    }

    switch (selector) {
    case kISCSIKitMethodRegisterCallback:
        if (!arguments->completion) {
            return kIOReturnBadArgument;
        }
        OSSafeReleaseNULL(ivars->pendingCallback);
        arguments->completion->retain();
        ivars->pendingCallback = arguments->completion;
        LOG("callback registered");
        return kIOReturnSuccess;

    case kISCSIKitMethodRegisterTarget:
        if (arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        return ivars->controller->DaemonRegisterTarget(arguments->scalarInput[0]);

    case kISCSIKitMethodUnregisterTarget:
        if (arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        return ivars->controller->DaemonUnregisterTarget(arguments->scalarInput[0]);

    case kISCSIKitMethodDequeueTask:
        if (arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        return ivars->controller->DaemonDequeueTask(arguments->scalarInput[0], arguments);

    case kISCSIKitMethodCompleteTask: {
        if (!arguments->structureInput) {
            return kIOReturnBadArgument;
        }
        const void * bytes = arguments->structureInput->getBytesNoCopy();
        uint64_t length = arguments->structureInput->getLength();
        if (!bytes) {
            return kIOReturnBadArgument;
        }
        return ivars->controller->DaemonCompleteTask(bytes, length);
    }

    default:
        return kIOReturnBadArgument;
    }
}
