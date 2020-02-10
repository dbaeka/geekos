/*
 * GeekOS interrupt handling data structures and functions
 * Copyright (c) 2001, David H. Hovemeyer <daveho@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 *
 * $Revision: 1.12 $
 * 
 */

/*
 * This module describes the C interface which must be implemented
 * by interrupt handlers, and has the initialization function
 * for the interrupt system as a whole.
 */

#ifndef GEEKOS_INT_H
#define GEEKOS_INT_H

#include <geekos/kassert.h>
#include <geekos/ktypes.h>
#include <geekos/defs.h>
#include <geekos/lock.h>

/*
 * This struct reflects the contents of the stack when
 * a C interrupt handler function is called.
 * It must be kept up to date with the code in "lowlevel.asm".
 */
struct Interrupt_State {
    /*
     * The register contents at the time of the exception.
     * We save these explicitly.
     */
    uint_t gs;
    uint_t fs;
    uint_t es;
    uint_t ds;
    uint_t ebp;
    uint_t edi;
    uint_t esi;
    uint_t edx;
    uint_t ecx;
    uint_t ebx;
    uint_t eax;

    /*
     * We explicitly push the interrupt number.
     * This makes it easy for the handler function to determine
     * which interrupt occurred.
     */
    uint_t intNum;

    /*
     * This may be pushed by the processor; if not, we push
     * a dummy error code, so the stack layout is the same
     * for every type of interrupt.
     */
    uint_t errorCode;

    /* These are always pushed on the stack by the processor. */
    uint_t eip;
    uint_t cs;
    uint_t eflags;
};

/*
 * An interrupt that occurred in user mode.
 * If Is_User_Interrupt(state) returns true, then the
 * Interrupt_State object may be cast to this kind of struct.
 */
struct User_Interrupt_State {
    struct Interrupt_State state;
    uint_t espUser;
    uint_t ssUser;
};


#ifdef GEEKOS                   /* stuff above may be used by user code. not obviously useful. */

static __inline__ bool Is_User_Interrupt(struct Interrupt_State *state) {
    return (state->cs & 3) == USER_PRIVILEGE;
}

/*
 * The interrupt flag bit in the eflags register.
 * FIXME: should be in something like "cpu.h".
 */
#define EFLAGS_IF (1 << 9)

/*
 * The signature of an interrupt handler.
 */
typedef void (*Interrupt_Handler) (struct Interrupt_State * state);

/*
 * Perform all low- and high-level initialization of the
 * interrupt system.
 */
void Init_Interrupts(int secondaryCPU);

/*
 * Query whether or not interrupts are currently enabled.
 */
bool Interrupts_Enabled(void);

Spin_Lock_t intLock;

extern void lockKernel();
extern void unlockKernel();
extern bool Kernel_Is_Locked(); /* it is locked, perhaps by another thread or inherited */
extern bool I_Locked_The_Kernel();      /* it is locked, definitely by me */

/*
 * Block interrupts.  While making geekos speak multiple
 * processors, a great big lock was acquired in this process.
 * The pairing of the two is now deprecated and should be
 * removed over time toward an explicit Disable_Interrupts 
 * coupled with lockKernel() as needed.
 */
static __inline__ void __Deprecated_Disable_Interrupts(void) {
    __asm__ __volatile__("cli");        /* ns - reversed so that spin lock acquired first, */
    /* then put back after interrupted by handlers while lock held. */
    lockKernel();
}

static __inline__ void __Disable_Interrupts(void) {
    __asm__ __volatile__("cli");
}

#define Deprecated_Disable_Interrupts()		\
do {					\
    KASSERT(Interrupts_Enabled());	\
    __Deprecated_Disable_Interrupts();		\
} while (0)

#define Disable_Interrupts()		\
do {					\
    KASSERT(Interrupts_Enabled());	\
    __Disable_Interrupts();		\
} while (0)


/*
 * Unblock interrupts.
 */
static __inline__ void __Deprecated_Enable_Interrupts(void) {
    unlockKernel();
    __asm__ __volatile__("sti");
}

static __inline__ void __Enable_Interrupts(void) {
    __asm__ __volatile__("sti");
}

#define Deprecated_Enable_Interrupts()		\
do {					\
    KASSERT(!Interrupts_Enabled());	\
    KASSERT(Kernel_Is_Locked());	\
    __Deprecated_Enable_Interrupts();		\
} while (0)

#define Enable_Interrupts()		\
do {					\
    KASSERT(!Interrupts_Enabled());	\
    __Enable_Interrupts();		\
} while (0)

/*
 * Dump interrupt state struct to screen
 */
void Dump_Interrupt_State(struct Interrupt_State *state);

/**
 * Start interrupt-atomic region.
 * @return true if interrupts were enabled at beginning of call,
 * false otherwise.
 */
static __inline__ bool Deprecated_Begin_Int_Atomic(void) {
    bool enabled = Interrupts_Enabled();
    if(enabled)
        Deprecated_Disable_Interrupts();
    else {
        /* ns 2014; this may be a bad idea, but it brings consistency to the migration
           of interrupt disabling === holding the great big lock in cases where only the
           interrupts are already disabled but the lock is not held. */
        if(!Kernel_Is_Locked()) {
            lockKernel();
        }
    }
    return enabled;
}

static __inline__ bool Begin_Int_Atomic(void) {
    bool interrupts_were_enabled = Interrupts_Enabled();
    if(interrupts_were_enabled)
        __Disable_Interrupts();
    return interrupts_were_enabled;
}

/**
 * End interrupt-atomic region.
 * @param iflag the value returned from the original Deprecated_Begin_Int_Atomic() call.
 */
static __inline__ void Deprecated_End_Int_Atomic(bool iflag) {
    KASSERT(!Interrupts_Enabled());
    if(iflag) {
        /* Interrupts were originally enabled, so turn them back on */
        Deprecated_Enable_Interrupts();
    }
}

static __inline__ void End_Int_Atomic(bool interrupts_were_enabled) {
    KASSERT(!Interrupts_Enabled());
    if(interrupts_were_enabled) {
        __Enable_Interrupts();
    }
}

#endif /* GEEKOS */
#endif /* GEEKOS_INT_H */
