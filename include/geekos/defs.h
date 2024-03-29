/*
 * Misc. kernel definitions
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
 * $Revision: 1.11 $
 * 
 */

#ifndef GEEKOS_DEFS_H
#define GEEKOS_DEFS_H

/*
 * Kernel code and data segment selectors.
 * Keep these up to date with defs.asm.
 */
#define KERNEL_CS  (1<<3)
#define KERNEL_DS  (2<<3)
#define KERNEL_GS  (3<<3)

/*
 * Pages for initial kernel thread context object and stack.
 * Keep these up to date with defs.asm.
 */
#define KERN_THREAD_OBJ (1024 * 1024)
#define KERN_STACK (KERN_THREAD_OBJ + 4096)

/*
 * Address where kernel is loaded
 */
#define KERNEL_START_ADDR 0x10000

/*
 * Kernel and user privilege levels
 */
#define KERNEL_PRIVILEGE 0
#define USER_PRIVILEGE 3


/*
 * Software interrupt for syscalls
 */
#define SYSCALL_INT 0x90

/*
 * The windows versions of gcc use slightly different
 * names for the bss begin and end symbols than the Linux version.
 */
#if defined(GNU_WIN32)
#  define BSS_START _bss_start__
#  define BSS_END _bss_end__
#else
#  define BSS_START __bss_start
#  define BSS_END end
#endif

/*
 * x86 has 4096 byte pages
 */
#define PAGE_POWER 12
#define PAGE_SIZE (1<<PAGE_POWER)
#define PAGE_MASK (~(0xffffffff << PAGE_POWER))

#endif /* GEEKOS_DEFS_H */
