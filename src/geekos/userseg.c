/*
 * Segmentation-based user mode implementation
 * Copyright (c) 2001,2003,2004 David H. Hovemeyer <daveho@cs.umd.edu>
 * Copyright (c) 2003,2013,2014 Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */

#include <geekos/ktypes.h>
#include <geekos/kassert.h>
#include <geekos/defs.h>
#include <geekos/mem.h>
#include <geekos/string.h>
#include <geekos/malloc.h>
#include <geekos/int.h>
#include <geekos/gdt.h>
#include <geekos/segment.h>
#include <geekos/tss.h>
#include <geekos/kthread.h>
#include <geekos/argblock.h>
#include <geekos/user.h>
#include <geekos/smp.h>

/* ----------------------------------------------------------------------
 * Variables
 * ---------------------------------------------------------------------- */

#define DEFAULT_USER_STACK_SIZE 8192


int userDebug = 0;

extern int CPU_Count;

/* ----------------------------------------------------------------------
 * Private functions
 * ---------------------------------------------------------------------- */

void *User_To_Kernel(struct User_Context *userContext, ulong_t userPtr) {
    uchar_t *userBase = (uchar_t *) userContext->memory;

    return (void *) (userBase + userPtr);
}

/*
 * Create a new user context of given size
 */
extern struct User_Context *Create_User_Context(ulong_t size) {
    struct User_Context *context;
    int index;

    /* Size must be a multiple of the page size */
    size = Round_Up_To_Page(size);
    if (userDebug)
        Print("Size of user memory == %lu (%lx) (%lu pages)\n", size,
              size, size / PAGE_SIZE);

    /* Allocate memory for the user context */
    context = (struct User_Context *) Malloc(sizeof(*context));
    if (context != 0) {
        memset(context, 0, sizeof(struct User_Context));
        context->memory = Malloc(size);
    }

    if (context == 0 || context->memory == 0)
        goto fail;

    /*
     * Fill user memory with zeroes;
     * leaving it uninitialized is a potential security flaw
     */
    memset(context->memory, '\0', size);

    context->size = size;

    /* Allocate an LDT descriptor for the user context */
    int i;
    for (i = 0; i < CPU_Count; i++) {
        context->ldtDescriptor[i] = Allocate_Segment_Descriptor_On_CPU(i);
        if (context->ldtDescriptor[i] == 0)
            goto fail;
        if (userDebug)
            Print("Allocated descriptor %d for LDT\n",
                  Get_Descriptor_Index(context->ldtDescriptor[i]));
        Init_LDT_Descriptor(context->ldtDescriptor[i], context->ldt,
                            NUM_USER_LDT_ENTRIES);
    }

    index = Get_Descriptor_Index(context->ldtDescriptor[0]);
    context->ldtSelector = Selector(KERNEL_PRIVILEGE, true, index);

    /* Initialize code and data segments within the LDT */
    Init_Code_Segment_Descriptor(&context->ldt[0],
                                 (ulong_t) context->memory,
                                 size / PAGE_SIZE, USER_PRIVILEGE);
    Init_Data_Segment_Descriptor(&context->ldt[1],
                                 (ulong_t) context->memory,
                                 size / PAGE_SIZE, USER_PRIVILEGE);
    context->csSelector = Selector(USER_PRIVILEGE, false, 0);
    context->dsSelector = Selector(USER_PRIVILEGE, false, 1);

    /* Nobody is using this user context yet */
    context->refCount = 0;

    for (i = 0; i < MAXSIG + 1; i++) {
        if (i == 0)
            context->signalTable[i] = 0;
        else
            context->signalTable[i] = SIG_DFL;
        context->receivedSignals[i] = false;
    }

    context->signalTable[SIGCHLD] = SIG_IGN;

    context->trampFunction = 0;

    context->busy = false;

    context->currentSignal = 0;

    /* Success! */
    return context;

    fail:
    /* We failed; release any allocated memory */
    if (context != 0) {
        if (context->memory != 0)
            Free(context->memory);
        Free(context);
    }

    return 0;
}

bool Validate_User_Memory(struct User_Context *userContext,
                          ulong_t userAddr, ulong_t bufSize,
                          int for_writing) {
    ulong_t avail;
    for_writing = for_writing;  /* avoid warning */

    if (userAddr >= userContext->size)
        return false;

    avail = userContext->size - userAddr;
    if (bufSize > avail)
        return false;

    return true;
}

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

/*
 * Destroy a User_Context object, including all memory
 * and other resources allocated within it.
 */
void Destroy_User_Context(struct User_Context *userContext) {
    KASSERT(userContext->refCount == 0);

    /* Free the context's LDT descriptor */
    int i;
    for (i = 0; i < CPU_Count; i++)
        Free_Segment_Descriptor(userContext->ldtDescriptor[i]);

    /* Free the context's memory */
    Free(userContext->memory);
    Free(userContext);
}

/*
 * Load a user executable into memory by creating a User_Context
 * data structure.
 * Params:
 * exeFileData - a buffer containing the executable to load
 * exeFileLength - number of bytes in exeFileData
 * exeFormat - parsed ELF segment information describing how to
 *   load the executable's text and data segments, and the
 *   code entry point address
 * command - string containing the complete command to be executed:
 *   this should be used to create the argument block for the
 *   process
 * pUserContext - reference to the pointer where the User_Context
 *   should be stored
 *
 * Returns:
 *   0 if successful, or an error code (< 0) if unsuccessful
 */
int Load_User_Program(char *exeFileData, ulong_t exeFileLength
__attribute__ ((unused)),
                      struct Exe_Format *exeFormat, const char *command,
                      struct User_Context **pUserContext) {
    int i;
    ulong_t maxva = 0;
    unsigned numArgs;
    ulong_t argBlockSize;
    ulong_t size, argBlockAddr;
    struct User_Context *userContext = 0;

    /* Find maximum virtual address */
    for (i = 0; i < exeFormat->numSegments; ++i) {
        struct Exe_Segment *segment = &exeFormat->segmentList[i];
        ulong_t topva = segment->startAddress + segment->sizeInMemory;  /* FIXME: range check */

        if (topva > maxva)
            maxva = topva;
    }

    /* Determine size required for argument block */
    Get_Argument_Block_Size(command, &numArgs, &argBlockSize);

    /*
     * Now we can determine the size of the memory block needed
     * to run the process.
     */
    size = Round_Up_To_Page(maxva) + DEFAULT_USER_STACK_SIZE;
    argBlockAddr = size;
    size += argBlockSize;

    /* Create User_Context */
    userContext = Create_User_Context(size);
    if (userContext == 0)
        return -1;

    /* Load segment data into memory */
    for (i = 0; i < exeFormat->numSegments; ++i) {
        struct Exe_Segment *segment = &exeFormat->segmentList[i];

        memcpy(userContext->memory + segment->startAddress,
               exeFileData + segment->offsetInFile,
               segment->lengthInFile);
    }

    /* Format argument block */
    Format_Argument_Block(userContext->memory + argBlockAddr, numArgs,
                          argBlockAddr, command);

    /* Fill in code entry point */
    userContext->entryAddr = exeFormat->entryAddr;

    /*
     * Fill in addresses of argument block and stack
     * (They happen to be the same)
     */
    userContext->argBlockAddr = argBlockAddr;
    userContext->stackPointerAddr = argBlockAddr;


    *pUserContext = userContext;
    return 0;
}

/*
 * Copy data from user memory into a kernel buffer.
 * Params:
 * destInKernel - address of kernel buffer
 * srcInUser - address of user buffer
 * bufSize - number of bytes to copy
 *
 * Returns:
 *   true if successful, false if user buffer is invalid (i.e.,
 *   doesn't correspond to memory the process has a right to
 *   access)
 */
bool Copy_From_User(void *destInKernel, ulong_t srcInUser,
                    ulong_t bufSize) {
    struct User_Context *current = CURRENT_THREAD->userContext;

    if (!Validate_User_Memory(current, srcInUser, bufSize, VUM_READING))
        return false;
    memcpy(destInKernel, User_To_Kernel(current, srcInUser), bufSize);

    return true;
}

/*
 * Copy data from kernel memory into a user buffer.
 * Params:
 * destInUser - address of user buffer
 * srcInKernel - address of kernel buffer
 * bufSize - number of bytes to copy
 *
 * Returns:
 *   true if successful, false if user buffer is invalid (i.e.,
 *   doesn't correspond to memory the process has a right to
 *   access)
 */
bool Copy_To_User(ulong_t destInUser, const void *srcInKernel,
                  ulong_t bufSize) {
    struct User_Context *current = CURRENT_THREAD->userContext;

    if (!Validate_User_Memory(current, destInUser, bufSize, VUM_WRITING))
        return false;
    memcpy(User_To_Kernel(current, destInUser), srcInKernel, bufSize);

    return true;
}

/*
 * Switch to user address space belonging to given
 * User_Context object.
 * Params:
 * userContext - the User_Context
 */
void Switch_To_Address_Space(struct User_Context *userContext) {
    ushort_t ldtSelector;

    /* Eager check to ensure that the new address space
       has either memory (userseg) or a page directory 
       (uservm) and is not likely to abort */
    KASSERT(userContext->memory || userContext->pageDir);

    /* Switch to the LDT of the new user context */
    ldtSelector = userContext->ldtSelector;
    __asm__ __volatile__("lldt %0"::"a"(ldtSelector)
    );
}
