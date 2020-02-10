/*
 * Block devices
 * Copyright (c) 2003 David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.17 $
 * 
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/errno.h>
#include <geekos/screen.h>
#include <geekos/string.h>
#include <geekos/malloc.h>
#include <geekos/int.h>
#include <geekos/kthread.h>
#include <geekos/synch.h>
#include <geekos/blockdev.h>
#include <geekos/kassert.h>

/* #define BLOCKDEV_DEBUG  */
#ifdef BLOCKDEV_DEBUG
#  define Debug(args...) Print(args)
#else
#  define Debug(args...)
#endif

extern Spin_Lock_t kthreadLock;

/* ----------------------------------------------------------------------
 * Private data and functions
 * ---------------------------------------------------------------------- */

/*
 * Lock protecting access/modification of block device list.
 */
static struct Mutex s_blockdevLock;

/* One mutex and condition variable for all requests */
/* Prior wait and request queues were per-device type rather
   than per-device; or were based on the globalLock. */
static struct Mutex s_blockdevRequestLock;
static struct Condition s_blockdevRequestCond;

/*
 * List datatype for list of block devices.
 */
DEFINE_LIST(Block_Device_List, Block_Device);
IMPLEMENT_LIST(Block_Device_List, Block_Device);

/*
 * The list in which all block devices in the system
 * are registered.
 */
static struct Block_Device_List s_deviceList;


/*
 * Perform a block IO request.
 * Returns 0 if successful, error code on failure.
 */
static int Do_Request(struct Block_Device *dev, enum Request_Type type,
                      int blockNum, void *buf) {
    struct Block_Request *request;
    int rc;

    // Print("about to do_req\n");
    Mutex_Lock(&s_blockdevLock);        /* not obviously the right mutex */
    request = Create_Request(dev, type, blockNum, buf);
    // Print("created req\n");
    if(request == 0) {
        // Print("req returned null\n");
        Mutex_Unlock(&s_blockdevLock);
        return ENOMEM;
    }
    // Print("about to post\n");
    Post_Request_And_Wait(request);
    rc = request->errorCode;
    Mutex_Unlock(&s_blockdevLock);
    Free(request);
    return rc;
}

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

/*
 * Register a block device.
 * This should be called by device drivers in their Init
 * functions to register all detected devices.
 * Returns 0 if successful, error code otherwise.
 */
int Register_Block_Device(const char *name, struct Block_Device_Ops *ops,
                          int unit, void *driverData,
                          struct Thread_Queue *waitQueue,
                          struct Block_Request_List *requestQueue) {
    struct Block_Device *dev;

    KASSERT(ops != 0);
    KASSERT(waitQueue != 0);
    KASSERT(requestQueue != 0);

    dev = (struct Block_Device *)Malloc(sizeof(*dev));
    if(dev == 0)
        return ENOMEM;

    strcpy(dev->name, name);
    dev->ops = ops;
    dev->unit = unit;
    dev->inUse = false;
    dev->driverData = driverData;
    dev->requestQueue = requestQueue;
    dev->reads = dev->writes = 0;
    dev->waitQueue = waitQueue; /* can be shared */
    dev->requestQueue = requestQueue;   /* can be shared */

    Mutex_Lock(&s_blockdevLock);
    /* FIXME: handle name conflict with existing device */
    Debug("Registering block device %s\n", dev->name);
    Add_To_Back_Of_Block_Device_List(&s_deviceList, dev);
    Mutex_Unlock(&s_blockdevLock);

    return 0;
}

/*
 * Open a named block device.
 * Return 0 if successful, error code on error.
 */
int Open_Block_Device(const char *name, struct Block_Device **pDev) {
    struct Block_Device *dev;
    int rc = 0;

    Mutex_Lock(&s_blockdevLock);

    dev = Get_Front_Of_Block_Device_List(&s_deviceList);
    while (dev != 0) {
        if(strcmp(dev->name, name) == 0)
            break;
        dev = Get_Next_In_Block_Device_List(dev);
    }

    if(dev == 0)
        rc = ENODEV;
    else if(dev->inUse)
        rc = EBUSY;
    else {
        rc = dev->ops->Open(dev);
        if(rc == 0) {
            *pDev = dev;
            dev->inUse = true;
        }
    }

    Mutex_Unlock(&s_blockdevLock);

    return rc;
}

/*
 * Close given block device.
 * Return 0 if successful, error code on error.
 */
int Close_Block_Device(struct Block_Device *dev) {
    int rc;

    Mutex_Lock(&s_blockdevLock);

    KASSERT(dev->inUse);
    rc = dev->ops->Close(dev);
    if(rc == 0)
        dev->inUse = false;

    Mutex_Unlock(&s_blockdevLock);

    return rc;
}

/*
 * Create a block device request to transfer a single block.
 */
struct Block_Request *Create_Request(struct Block_Device *dev,
                                     enum Request_Type type, int blockNum,
                                     void *buf) {
    struct Block_Request *request = Malloc(sizeof(*request));
    if(request != 0) {
        memset(request, 0, sizeof(struct Block_Request));       /* must bzero waitQueue */
        request->dev = dev;
        request->type = type;
        request->blockNum = blockNum;
        request->buf = buf;
        request->state = PENDING;
        //      Clear_Thread_Queue(&request->waitQueue);
        Cond_Init(&request->satisfied);
    }
    return request;
}

/*
 * Send a block IO request to a device and wait for it to be handled.
 * Returns when the driver completes the requests or signals
 * an error.
 *
 */
void Post_Request_And_Wait(struct Block_Request *request) {
    struct Block_Device *dev;

    KASSERT(request != 0);

    dev = request->dev;
    KASSERT(dev != 0);

    /* Send request to the driver */
    Debug("Posting block device request [@%p]...\n", request);

    Mutex_Lock(&s_blockdevRequestLock);
    Add_To_Back_Of_Block_Request_List(dev->requestQueue, request);
    Cond_Broadcast(&s_blockdevRequestCond);     /* awakens Dequeue_Request below */

    /* Wait for request to be processed */
    while (request->state == PENDING) {
        Debug("Waiting, state=%d\n", request->state);
        // Disable_Interrupts();
        // Wait(&request->waitQueue);
        Cond_Wait(&request->satisfied, &s_blockdevRequestLock);
        // Enable_Interrupts();
    }
    Mutex_Unlock(&s_blockdevRequestLock);
    Debug("Wait completed!\n");
}

/*
 * Wait for a block request to arrive.
 */
struct Block_Request *Dequeue_Request(struct Block_Request_List
                                      *requestQueue) {
    struct Block_Request *request;

    Mutex_Lock(&s_blockdevRequestLock);

    while (!
           (request =
            Remove_From_Front_Of_Block_Request_List(requestQueue))) {
        /* could have something added here by Post_Request_And_Wait */
        Cond_Wait(&s_blockdevRequestCond, &s_blockdevRequestLock);
    }

    Mutex_Unlock(&s_blockdevRequestLock);

    return request;
}

/*
 * Signal the completion of a block request.
 */
void Notify_Request_Completion(struct Block_Request *request,
                               enum Request_State state, int errorCode) {
    request->state = state;
    request->errorCode = errorCode;
    Cond_Signal(&request->satisfied);
    // Print("+++ in Notify_Request_Completion: core %d\n", Get_CPU_ID()); 
    // Disable_Interrupts();
    // Wake_Up(&request->waitQueue);
    // Print("--- in Notify_Request_Completion: core %d\n", Get_CPU_ID()); 
    // Enable_Interrupts();
    /* Ensure that Wake_Up did its job; this should never fail. */
    // KASSERT(Is_Thread_Queue_Empty(&request->waitQueue));
}

/*
 * Read a block from given device.
 * Return 0 if successful, error code on error.
 */
int Block_Read(struct Block_Device *dev, int blockNum, void *buf) {
    KASSERT(dev);
    KASSERT(buf);
    dev->reads++;
    return Do_Request(dev, BLOCK_READ, blockNum, buf);
}

/*
 * Write a block to given device.
 * Return 0 if successful, error code on error.
 */
int Block_Write(struct Block_Device *dev, int blockNum, void *buf) {
    KASSERT(dev);
    KASSERT(buf);
    dev->writes++;
    return Do_Request(dev, BLOCK_WRITE, blockNum, buf);
}

/*
 * Get number of blocks in given device.
 */
int Get_Num_Blocks(struct Block_Device *dev) {
    return dev->ops->Get_Num_Blocks(dev);
}

/* 
 * Dump block device statistics, mostly for performance diagnostics.
 */
void Dump_Blockdev_Stats(void) {
    struct Block_Device *dev;
    int i;
    Print("Block Device Stats:\n");
    Mutex_Lock(&s_blockdevLock);
    for(dev = Get_Front_Of_Block_Device_List(&s_deviceList), i = 5;
        dev != 0 && i > 0;
        dev = Get_Next_In_Block_Device_List(dev), i -= 1) {
        Print(" %s: read %u wrote %u\n", dev->name, dev->reads,
              dev->writes);
    }
    Mutex_Unlock(&s_blockdevLock);
}
