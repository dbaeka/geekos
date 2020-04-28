/*
 * Paging (virtual memory) support
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
 * $Revision: 1.56 $
 * 
 */

#include <geekos/string.h>
#include <geekos/int.h>
#include <geekos/idt.h>
#include <geekos/kthread.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/mem.h>
#include <geekos/malloc.h>
#include <geekos/gdt.h>
#include <geekos/segment.h>
#include <geekos/user.h>
#include <geekos/vfs.h>
#include <geekos/crc32.h>
#include <geekos/paging.h>
#include <geekos/errno.h>
#include <geekos/projects.h>
#include <geekos/smp.h>

#include <libc/mmap.h>

/* ----------------------------------------------------------------------
 * Public data
 * ---------------------------------------------------------------------- */
static pde_t *pageDirectory;
/* ----------------------------------------------------------------------
 * Private functions/data
 * ---------------------------------------------------------------------- */

#define SECTORS_PER_PAGE (PAGE_SIZE / SECTOR_SIZE)

/*
 * flag to indicate if debugging paging code
 */
int debugFaults = 0;
#define Debug(args...) if (debugFaults) Print(args)

/* address of local APIC - generally at the default address */
static char *const APIC_Addr = (char *) 0xFEE00000;

/* address of IO APIC - generally at the default address */
static char *const IO_APIC_Addr = (char *) 0xFEC00000;

#define UNMAP_ADDR(x)   (((unsigned int) (x)) << 12)

/* const because we do not expect any caller to need to
   modify the kernel page directory */
const pde_t *Kernel_Page_Dir(void) {
    return pageDirectory;
}


/*
 * Print diagnostic information for a page fault.
 */
static void Print_Fault_Info(uint_t address, faultcode_t faultCode) {
    extern uint_t g_freePageCount;
    struct Kernel_Thread *current = get_current_thread(
            0);      /* informational, could be incorrect with low probability */

    if (current) {
        Print("Pid %d: (%p/%s)", current->pid, current,
              current->threadName);
    }
    Print("\n Page Fault received, at address %p (%d pages free)\n",
          (void *) address, g_freePageCount);
    if (faultCode.protectionViolation)
        Print("   Protection Violation, ");
    else
        Print("   Non-present page, ");
    if (faultCode.writeFault)
        Print("Write Fault, ");
    else
        Print("Read Fault, ");
    if (faultCode.userModeFault)
        Print("in User Mode\n");
    else
        Print("in Supervisor Mode\n");
}

union type_pun_workaround {
    faultcode_t faultCode;
    ulong_t errorCode;
};

/*
 * Handler for page faults.
 * You should call the Install_Interrupt_Handler() function to
 * register this function as the handler for interrupt 14.
 */
/*static*/ void Page_Fault_Handler(struct Interrupt_State *state) {
    ulong_t address;
    union type_pun_workaround tpw;
    faultcode_t faultCode;

    KASSERT(!Interrupts_Enabled());

    /* Get the address that caused the page fault */
    address = Get_Page_Fault_Address();
    Debug("Page fault @%lx\n", address);


    if (address < 0xfec01000 && address > 0xf0000000) {
        Print("page fault address in APIC/IOAPIC range\n");
        goto error;
    }

    /* Get the fault code */
    tpw.errorCode = state->errorCode;
    faultCode = tpw.faultCode;
    // faultCode = *((faultcode_t *) &(state->errorCode));

    /* rest of your handling code here */
    TODO_P(PROJECT_VIRTUAL_MEMORY_B, "handle page faults");

    TODO_P(PROJECT_MMAP, "handle mmap'd page faults");


    error:
    Print("Unexpected Page Fault received\n");
    Print_Fault_Info(address, faultCode);
    Dump_Interrupt_State(state);
    /* user faults just kill the process; not user mode faults should halt the kernel. */
    KASSERT0(faultCode.userModeFault,
             "unhandled kernel-mode page fault.");

    /* For now, just kill the thread/process. */
    Enable_Interrupts();
    Exit(-1);
}

void Identity_Map_Page(pde_t *currentPageDir, unsigned int address,
                       int flags) {
    currentPageDir->pageTableBaseAddr = address >> 12;
    currentPageDir->flags = flags;
    currentPageDir->present = 1;
}

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */


/*
 * Initialize virtual memory by building page tables
 * for the kernel and physical memory.
 */
void Init_VM(struct Boot_Info *bootInfo) {
    /*
     * Hints:
     * - Build kernel page directory and page tables
     * - Call Enable_Paging() with the kernel page directory
     * - Install an interrupt handler for interrupt 14,
     *   page fault
     * - Do not map a page at address 0; this will help trap
     *   null pointer references
     */
    ulong_t i;
    ulong_t bytes = bootInfo->memSizeKB << 10;
    ulong_t lastPageVAddr = Round_Up_To_Page(bytes);
    ulong_t startPageAddr = 0x0;

    pageDirectory = Alloc_Page();
    memset(pageDirectory, 0, NUM_PAGE_DIR_ENTRIES * sizeof(pde_t));

    for (i = startPageAddr; i < lastPageVAddr; i += PAGE_SIZE) {
        ulong_t pDirIdx = PAGE_DIRECTORY_INDEX((ulong_t) i);
        ulong_t pTableIdx = PAGE_TABLE_INDEX((ulong_t) i);
        pde_t *pageDir = &pageDirectory[pDirIdx];
        if (!pageDir->present) {
            pte_t *pageTable = Alloc_Page();
            memset(pageTable, 0, NUM_PAGE_TABLE_ENTRIES * sizeof(pte_t));
            Identity_Map_Page(pageDir, (uint_t) pageTable, VM_READ | VM_WRITE);
            if (PAGE_ALIGNED_ADDR(i) != 0) {
                pageTable[pTableIdx].pageBaseAddr = PAGE_ALIGNED_ADDR(i);
                pageTable[pTableIdx].flags = VM_READ | VM_WRITE;
                pageTable[pTableIdx].present = 1;
            }
        } else {
            pte_t *pageTable = (pte_t *) ((ulong_t) UNMAP_ADDR(pageDir->pageTableBaseAddr));
            pageTable[pTableIdx].pageBaseAddr = PAGE_ALIGNED_ADDR(i);
            pageTable[pTableIdx].flags = VM_READ | VM_WRITE;
            pageTable[pTableIdx].present = 1;
        }
    }

    pte_t *pageTable = Alloc_Page();
    memset(pageTable, 0, NUM_PAGE_TABLE_ENTRIES * sizeof(pte_t));

    int apid_dir = PAGE_DIRECTORY_INDEX((ulong_t) APIC_Addr);
    uint_t apic_pt = PAGE_TABLE_INDEX((ulong_t) APIC_Addr);
    uint_t ioapic_pt = PAGE_TABLE_INDEX((ulong_t) IO_APIC_Addr);

    Identity_Map_Page(&pageDirectory[apid_dir], (uint_t) pageTable, VM_READ | VM_WRITE);

    pageTable[apic_pt].flags = VM_WRITE | VM_READ;
    pageTable[apic_pt].present = 1;
    pageTable[apic_pt].pageBaseAddr = (uint_t) PAGE_ALIGNED_ADDR((int) APIC_Addr);

    pageTable[ioapic_pt].flags = VM_WRITE | VM_READ;
    pageTable[ioapic_pt].present = 1;
    pageTable[ioapic_pt].pageBaseAddr = (uint_t) PAGE_ALIGNED_ADDR((int) IO_APIC_Addr);

    Enable_Paging(pageDirectory);
    Install_Interrupt_Handler(14, Page_Fault_Handler);
}

void Init_Secondary_VM() {
    Enable_Paging(pageDirectory);
}

/**
 * Initialize paging file data structures.
 * All filesystems should be mounted before this function
 * is called, to ensure that the paging file is available.
 */
void Init_Paging(void) {
    TODO_P(PROJECT_VIRTUAL_MEMORY_B,
           "Initialize paging file data structures");
}

/* guards your structure for tracking free space on the paging file. */
static Spin_Lock_t s_free_space_spin_lock;

/**
 * Find a free bit of disk on the paging file for this page.
 * Interrupts must be disabled.
 * @return index of free page sized chunk of disk space in
 *   the paging file, or -1 if the paging file is full
 */
int Find_Space_On_Paging_File(void) {
    unsigned int retval;
    int iflag = Begin_Int_Atomic();
    Spin_Lock(&s_free_space_spin_lock);
    TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Find free page in paging file");
    retval = EUNSUPPORTED;
    Spin_Unlock(&s_free_space_spin_lock);
    End_Int_Atomic(iflag);
    return retval;
}

/**
 * Free a page-sized chunk of disk space in the paging file.
 * Interrupts must be disabled.
 * @param pagefileIndex index of the chunk of disk space
 */
void Free_Space_On_Paging_File(int pagefileIndex) {
    int iflag = Begin_Int_Atomic();
    Spin_Lock(&s_free_space_spin_lock);
    TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Free page in paging file");
    Spin_Unlock(&s_free_space_spin_lock);
    End_Int_Atomic(iflag);
}

/**
 * Write the contents of given page to the indicated block
 * of space in the paging file.
 * @param paddr a pointer to the physical memory of the page
 * @param vaddr virtual address where page is mapped in user memory
 * @param pagefileIndex the index of the page sized chunk of space
 *   in the paging file
 */
void Write_To_Paging_File(void *paddr, ulong_t vaddr, int pagefileIndex) {
    struct Page *page = Get_Page((ulong_t) paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE));    /* Page must be pageable! */
    KASSERT(page->flags & PAGE_LOCKED); /* Page must be locked! */
    TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Write page data to paging file");
}

/**
 * Read the contents of the indicated block
 * of space in the paging file into the given page.
 * @param paddr a pointer to the physical memory of the page
 * @param vaddr virtual address where page will be re-mapped in
 *   user memory
 * @param pagefileIndex the index of the page sized chunk of space
 *   in the paging file
 */
void Read_From_Paging_File(void *paddr, ulong_t vaddr, int pagefileIndex) {
    struct Page *page = Get_Page((ulong_t) paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE));    /* Page must be locked! */
    TODO_P(PROJECT_VIRTUAL_MEMORY_B, "Read page data from paging file");
}


void *Mmap_Impl(void *ptr, unsigned int length, int prot, int flags,
                int fd) {
    TODO_P(PROJECT_MMAP, "Mmap setup mapping");
    return NULL;
}

bool Is_Mmaped_Page(struct User_Context *context, ulong_t vaddr) {
    TODO_P(PROJECT_MMAP,
           "is this passed vaddr an mmap'd page in the passed user context");
    return false;
}

void Write_Out_Mmaped_Page(struct User_Context *context, ulong_t vaddr) {
    TODO_P(PROJECT_MMAP, "Mmap write back dirty mmap'd page");
}

int Munmap_Impl(ulong_t ptr) {
    TODO_P(PROJECT_MMAP, "unmapp the pages");
    return 0;
}
