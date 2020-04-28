/*
 * Paging-based user mode implementation
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
 *
 * $Revision: 1.51 $
 */

#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/paging.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/argblock.h>
#include <geekos/kthread.h>
#include <geekos/range.h>
#include <geekos/vfs.h>
#include <geekos/user.h>
#include <geekos/projects.h>
#include <geekos/smp.h>
#include <geekos/synch.h>
#include <geekos/errno.h>
#include <geekos/elf.h>
#include <geekos/gdt.h>

extern Spin_Lock_t kthreadLock;

extern int CPU_Count;

#define DEFAULT_USER_STACK_SIZE 8192
#define USER_VM_SIZE 0x70000000
#define UNMAP_ADDR(x)   (((unsigned int) (x)) << 12)

int userDebug = 0;
static char *const USER_BASE = (char *) 0x80000000;
static char *const APIC_Addr = (char *) 0xFEE00000;
ulong_t USER_LINSIZE = 0x70000000;
#define Debug(args...) if (userDebug) Print("uservm: " args)

/* ----------------------------------------------------------------------
 * Private functions
 * ---------------------------------------------------------------------- */

void *User_To_Kernel(struct User_Context *userContext, ulong_t userPtr) {
    uchar_t *userBase = (uchar_t *) userContext->memory;

    return (void *) (userBase + userPtr);
}

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
        context->memory = USER_BASE;
    }

    if (context == 0 || context->memory == 0)
        goto fail;

    /*
     * Fill user memory with zeroes;
     * leaving it uninitialized is a potential security flaw
     */
//    memset(context->memory, '\0', size);

    context->size = USER_LINSIZE;

    // Init Page Dir
    context->pageDir = Alloc_Page();

    if (context->pageDir == 0)
        goto fail;

    const pde_t *kernelPageDir = Kernel_Page_Dir();
    memset(context->pageDir, 0, NUM_PAGE_DIR_ENTRIES * sizeof(pde_t));
    memcpy(context->pageDir, kernelPageDir, sizeof(pde_t) * NUM_PAGE_DIR_ENTRIES / 2);
    int apid_dir = PAGE_DIRECTORY_INDEX((ulong_t) APIC_Addr);
    memcpy(&context->pageDir[apid_dir], &kernelPageDir[apid_dir], sizeof(pde_t));

    /* Allocate an LDT descriptor for the user context */
    context->ldtDescriptor = Allocate_Segment_Descriptor();
    if (context->ldtDescriptor == 0)
        goto fail;
    if (userDebug)
        Print("Allocated descriptor %d for LDT\n",
              Get_Descriptor_Index(context->ldtDescriptor));
    Init_LDT_Descriptor(context->ldtDescriptor, context->ldt,
                        NUM_USER_LDT_ENTRIES);
    index = Get_Descriptor_Index(context->ldtDescriptor);
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


/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */



void protectPages(struct User_Context *userContext, ulong_t begin_user_dir, ulong_t last_user_dir, ulong_t numPages,
                  bool isLock) {
    ulong_t i, j;
    ulong_t pageNum = numPages;
    Disable_Interrupts();
    for (i = begin_user_dir; i < last_user_dir; i++) {
        pde_t pageDir = userContext->pageDir[i];
        pte_t *pageTable = (pte_t *) UNMAP_ADDR((ulong_t) pageDir.pageTableBaseAddr);

        int pageTablesRem = (last_user_dir - i);
        ulong_t max = (pageTablesRem != 1) ? NUM_PAGE_TABLE_ENTRIES : pageNum;
        for (j = 1; j < max; j++) {
            struct Page *page = (struct Page *) UNMAP_ADDR((ulong_t) pageTable[j].pageBaseAddr);
            page->flags = (isLock) ? page->flags & ~PAGE_PAGEABLE : page->flags | PAGE_PAGEABLE;
        }
        pageNum -= NUM_PAGE_TABLE_ENTRIES;
    }
    Enable_Interrupts();
}

/*
 * Destroy a User_Context object, including all memory
 * and other resources allocated within it.
 */
void Destroy_User_Context(struct User_Context *userContext) {
    /*
     * Hints:
     * - Free all pages, page tables, and page directory for
     *   the process (interrupts must be disabled while you do this,
     *   otherwise those pages could be stolen by other processes)
     * - Free semaphores, files, and other resources used
     *   by the process
     */
    KASSERT(userContext->refCount == 0);

    /* Free the context's LDT descriptor */
    ulong_t i;
    Disable_Interrupts();


    pde_t *pageDir = userContext->pageDir;

    ulong_t lastUserSpaceAddr = Round_Up_To_Page((ulong_t) userContext->memory + userContext->size);
    for (i = (ulong_t) userContext->memory; i < lastUserSpaceAddr; i += PAGE_SIZE) {
        ulong_t pDirIdx = PAGE_DIRECTORY_INDEX((ulong_t) i);
        ulong_t pTableIdx = PAGE_TABLE_INDEX((ulong_t) i);
        pde_t pageDir = userContext->pageDir[pDirIdx];
        if (pageDir.present) {
            pte_t *pageTable = (pte_t *) ((ulong_t) UNMAP_ADDR(pageDir.pageTableBaseAddr));
            if (pageTable[pTableIdx].present) {
                struct Page *page = (struct Page *) UNMAP_ADDR((ulong_t) pageTable[pTableIdx].pageBaseAddr);
                if (page != NULL)
                    Free_Page(page);
//              TODO:  Free_Page((void *) ((ulong_t) pageTable[pTableIdx]));
            }
        }
    }

    Free_Page(pageDir);

    Enable_Interrupts();
    /* Free the context's LDT descriptor */
    Free_Segment_Descriptor(userContext->ldtDescriptor);

    /* Free the context's memory */
//    Free(userContext->memory);
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
int Load_User_Program(char *exeFileData, ulong_t exeFileLength,
                      struct Exe_Format *exeFormat, const char *command,
                      struct User_Context **pUserContext) {
    /*
     * Hints:
     * - This will be similar to the same function in userseg.c
     * - Determine space requirements for code, data, argument block,
     *   and stack
     * - Allocate pages for above, map them into user address
     *   space (allocating page directory and page tables as needed)
     * - Fill in initial stack pointer, argument block address,
     *   and code entry point fields in User_Context
     */

    ulong_t i, j;
    int k;
    ulong_t maxva = 0;
    unsigned numArgs;
    ulong_t argBlockSize;
    ulong_t size, argBlockAddr;
    struct User_Context *userContext = 0;

    /* Find maximum virtual address */
    for (k = 0; k < exeFormat->numSegments; ++k) {
        struct Exe_Segment *segment = &exeFormat->segmentList[k];
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

    if (size > USER_VM_SIZE)
        return ENOMEM;

    //TODO; might need to check argblocksize

    ulong_t numPages = size / PAGE_SIZE + 1;

    /* Create User_Context */
    userContext = Create_User_Context(size);
    if (userContext == 0)
        return -1;

    ulong_t begin_user_dir = PAGE_DIRECTORY_INDEX((ulong_t) userContext->memory);
    ulong_t numPageTables = numPages / NUM_PAGE_TABLE_ENTRIES + 1;

    ulong_t tmpNumPages = numPages;
    for (i = begin_user_dir; i < (numPageTables + begin_user_dir); i++) {
        pte_t *pageTable = Alloc_Page();
        if (pageTable == 0)
            return ENOMEM;
        memset(pageTable, 0, NUM_PAGE_TABLE_ENTRIES * sizeof(pte_t));

        Identity_Map_Page(&userContext->pageDir[i], (uint_t) pageTable, VM_USER | VM_READ | VM_WRITE);

        ulong_t pageTablesRem = (numPageTables + begin_user_dir - i);
        ulong_t max = (pageTablesRem != 1) ? NUM_PAGE_TABLE_ENTRIES : tmpNumPages;
        for (j = 0; j < max; j++) {
            if (!(i == begin_user_dir && j == 0)) {
                pageTable[j].flags = VM_USER | VM_READ | VM_WRITE;
                pageTable[j].present = 1;
                uint_t vaddr = (((i - begin_user_dir) * NUM_PAGE_TABLE_ENTRIES) + (j - 1)) * PAGE_SIZE;
                void *pageMem = Alloc_Pageable_Page(&pageTable[j], vaddr);
                if (pageMem == 0)
                    return ENOMEM;
                pageTable[j].pageBaseAddr = PAGE_ALIGNED_ADDR((uint_t) pageMem);
            }
        }
        tmpNumPages -= NUM_PAGE_TABLE_ENTRIES;
    }


    /* Load segment data into memory */
    pde_t *oldPDBR = Get_PDBR();
    Set_PDBR((void *) userContext->pageDir);
//    protectPages(userContext, begin_user_dir, (begin_user_dir + numPageTables), numPages, true);
    for (k = 0; k < exeFormat->numSegments; ++k) {
        struct Exe_Segment *segment = &exeFormat->segmentList[k];
        memcpy(User_To_Kernel(userContext, segment->startAddress),
               exeFileData + segment->offsetInFile,
               segment->lengthInFile);
        if ((segment->protFlags & VM_WRITE) == 0) {
            ulong_t lastPageVAddr =
                    (ulong_t) userContext->memory + Round_Up_To_Page(segment->startAddress + segment->sizeInMemory);
            ulong_t startPageAddr = (ulong_t) userContext->memory + Round_Down_To_Page(segment->startAddress);
            for (i = startPageAddr; i < lastPageVAddr; i += PAGE_SIZE) {
                ulong_t pDirIdx = PAGE_DIRECTORY_INDEX((ulong_t) i);
                ulong_t pTableIdx = PAGE_TABLE_INDEX((ulong_t) i);
                pde_t pageDir = userContext->pageDir[pDirIdx];
                pte_t *pageTable = (pte_t *) ((ulong_t) UNMAP_ADDR(pageDir.pageTableBaseAddr));
                pageTable[pTableIdx].flags &= ~VM_WRITE;
            }
        }
    }
//    protectPages(userContext, begin_user_dir, (begin_user_dir + numPageTables), numPages, false);

    /* Format argument block */
    Format_Argument_Block(userContext->memory + argBlockAddr, numArgs,
                          argBlockAddr, command);

    Set_PDBR((void *) oldPDBR);
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


/*
 * Copy data from user buffer into kernel buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_From_User(void *destInKernel, ulong_t srcInUser,
                    ulong_t numBytes) {
    /*
     * Hints:
     * - Make sure that user page is part of a valid region
     *   of memory
     * - Remember that you need to add 0x80000000 to user addresses
     *   to convert them to kernel addresses, because of how the
     *   user code and data segments are defined
     * - User pages may need to be paged in from disk before being accessed.
     * - Before you touch (read or write) any data in a user
     *   page, **disable the PAGE_PAGEABLE bit**.
     *
     * Be very careful with race conditions in reading a page from disk.
     * Kernel code must always assume that if the struct Page for
     * a page of memory has the PAGE_PAGEABLE bit set,
     * IT CAN BE STOLEN AT ANY TIME.  The only exception is if
     * interrupts are disabled; because no other process can run,
     * the page is guaranteed not to be stolen.
     */
    struct User_Context *current = CURRENT_THREAD->userContext;

    if (!Validate_User_Memory(current, srcInUser, numBytes, VUM_READING))
        return false;
    pde_t *oldPDBR = Get_PDBR();
    Set_PDBR((void *) current->pageDir);
    ulong_t numPages = numBytes / PAGE_SIZE;
    ulong_t begin_user_dir = PAGE_DIRECTORY_INDEX((ulong_t) current->memory + srcInUser);
    ulong_t numPageTables = numPages / NUM_PAGE_TABLE_ENTRIES + 1;
    protectPages(current, begin_user_dir, (begin_user_dir + numPageTables), numPages, true);
    memcpy(destInKernel, User_To_Kernel(current, srcInUser), numBytes);
    protectPages(current, begin_user_dir, (begin_user_dir + numPageTables), numPages, false);
    Set_PDBR((void *) oldPDBR);
    return true;
}

/*
 * Copy data from kernel buffer into user buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_To_User(ulong_t destInUser, const void *srcInKernel,
                  ulong_t numBytes) {
    /*
     * Hints:
     * - Same as for Copy_From_User()
     * - TODO: Also, make sure the memory is mapped into the user
     *   address space with write permission enabled
     */
    struct User_Context *current = CURRENT_THREAD->userContext;

    if (!Validate_User_Memory(current, destInUser, numBytes, VUM_WRITING))
        return false;
    pde_t *oldPDBR = Get_PDBR();
    Set_PDBR((void *) current->pageDir);
    ulong_t numPages = numBytes / PAGE_SIZE;
    ulong_t begin_user_dir = PAGE_DIRECTORY_INDEX((ulong_t) current->memory + destInUser);
    ulong_t numPageTables = numPages / NUM_PAGE_TABLE_ENTRIES + 1;
    protectPages(current, begin_user_dir, (begin_user_dir + numPageTables), numPages, true);
    memcpy(User_To_Kernel(current, destInUser), srcInKernel, numBytes);
    protectPages(current, begin_user_dir, (begin_user_dir + numPageTables), numPages, false);
    Set_PDBR((void *) oldPDBR);
    return true;
}


/*
 * Switch to user address space.
 */
void Switch_To_Address_Space(struct User_Context *userContext) {
    /*
     * - If you are still using an LDT to define your user code and data
     *   segments, switch to the process's LDT
     * -
     */
    /* Eager check to ensure that the new address space
      has either memory (userseg) or a page directory
      (uservm) and is not likely to abort */
    ushort_t ldtSelector;

    KASSERT(userContext->memory || userContext->pageDir);

    Set_PDBR((void *) userContext->pageDir);

    /* Switch to the LDT of the new user context */
    ldtSelector = userContext->ldtSelector;
    __asm__ __volatile__("lldt %0"::"a"(ldtSelector)
    );
}
