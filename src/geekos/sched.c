#include <geekos/kassert.h>
#include <geekos/defs.h>
#include <geekos/screen.h>
#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/symbol.h>
#include <geekos/string.h>
#include <geekos/kthread.h>
#include <geekos/malloc.h>
#include <geekos/user.h>
#include <geekos/alarm.h>
#include <geekos/projects.h>
#include <geekos/smp.h>
#include <geekos/synch.h>

/* The lock associated with the run queue(s). */
static Spin_Lock_t run_queue_spinlock;

/* The centralized run queue of all threads that are ready to run. */
static struct Thread_Queue s_runQueue;

static struct Kernel_Thread *Get_Next_Runnable_Locked(void);
static void Make_Runnable_Locked(struct Kernel_Thread *kthread);

enum Scheduler { RR = 0,        /* default */
    MLFQ = 1,
    MPWS,
};
static enum Scheduler s_scheduler = RR;

/*
 * Add given thread to the run queue, so that it may be
 * scheduled.  Must be called with interrupts disabled!  Is
 * invoked on the current thread when the thread is
 * interrupted before passing control to another thread.
 * (Also invoked when switching schedulers.)
 */
static void Make_Runnable_Locked(struct Kernel_Thread *kthread) {
    Enqueue_Thread(&s_runQueue, kthread);
    TODO_P(PROJECT_SCHEDULING, "replace make runnable as needed");
}

void Make_Runnable(struct Kernel_Thread *kthread) {
    KASSERT(!Interrupts_Enabled());

    KASSERT0(kthread->inThread_Queue == NULL,
             "attempting to make runnable a thread that is in another list.");

    if(kthread->priority == PRIORITY_IDLE)
        return;                 /* idle handled oob ns14 */

    Spin_Lock(&run_queue_spinlock);

    Make_Runnable_Locked(kthread);

    Spin_Unlock(&run_queue_spinlock);
}

/*
 * Atomically make a thread runnable.
 */
void Make_Runnable_Atomic(struct Kernel_Thread *kthread) {
    int iflag = Begin_Int_Atomic();
    Make_Runnable(kthread);
    End_Int_Atomic(iflag);
}


/*
 * Find the best (highest priority) thread in given
 * thread queue.  Returns null if queue is empty.
 */
static __inline__ struct Kernel_Thread *Find_Best(struct Thread_Queue
                                                  *queue) {
    int cpuID;

    KASSERT(Is_Locked(&run_queue_spinlock));

    cpuID = Get_CPU_ID();

    /* Pick the highest priority thread */
    struct Kernel_Thread *kthread = queue->head, *best = 0;
    while (kthread != 0) {
        if(kthread->affinity == AFFINITY_ANY_CORE ||
           kthread->affinity == cpuID) {
            if(best == 0 || kthread->priority > best->priority)
                // if (kthread->alive) - must finish exiting if not alive.
                best = kthread;
        }
        kthread = Get_Next_In_Thread_Queue(kthread);
    }

    if(!best) {
        best = CPUs[cpuID].idleThread;
    }

    return best;
}

/*
 * Get the next runnable thread from the run queue.
 * This is the scheduler.
 */
struct Kernel_Thread *Get_Next_Runnable_Locked(void) {
    struct Kernel_Thread *best = 0;
    best = Find_Best(&s_runQueue);
    KASSERT(best != 0);
    if(best->priority != PRIORITY_IDLE) {       /* ns14 oob idle */
        Remove_Thread(&s_runQueue, best);
    }

    KASSERT(Is_Locked(&run_queue_spinlock));
    TODO_P(PROJECT_SCHEDULING, "fix Get_Next_Runnable");
    return best;
}

/* Called by lowlevel.asm in handle_interrupt, with
   interrupts disabled, but no locks held. */
struct Kernel_Thread *Get_Next_Runnable(void) {
    struct Kernel_Thread *ret;

    /* ns14 */
    //Deprecated_Enable_Interrupts();
    KASSERT(!Interrupts_Enabled());

#if 0
    if(!Try_Spin_Lock(&run_queue_spinlock)) {
        /* attempt at dealing with the potential problem of waiting for the
           kthread lock while holding the global lock... prefer to schedule
           the idle thread instead of deadlocking. */
        int cpuID = Get_CPU_ID();
        Print
            ("GNR returning Idle thread on cpu %d to %p due to run_queue_spinlock\n",
             cpuID, __builtin_return_address(0));
        return CPUs[cpuID].idleThread;
    }
#else
    Spin_Lock(&run_queue_spinlock);
#endif

    /* Spin_Lock(&run_queue_spinlock); */
    /* ns14 - hacking at getting this right, since we will need the kthreadlock
       for a little while, try to keep the processor longer */
    g_preemptionDisabled[Get_CPU_ID()] = true;

    ret = Get_Next_Runnable_Locked();
    // Print("about to run %d, esp = %x\n", ret->pid, ret->esp);

    Spin_Unlock(&run_queue_spinlock);

    /* ns14 */
    //Deprecated_Disable_Interrupts();
    g_preemptionDisabled[Get_CPU_ID()] = false;

    /* at least could be the idle thread */
    KASSERT(ret);

    /* ensure the new thread has a valid kernel stack pointer. */
    KASSERT(((unsigned long)(ret->esp - 1) & ~0xfff) ==
            ((unsigned long)ret->stackPage));

    return ret;
}


/* This helper function is meant to facilitate implementing PS */
int Is_Thread_On_Run_Queue(const struct Kernel_Thread *thread) {
    if(s_scheduler == RR) {
        return Is_Member_Of_Thread_Queue(&s_runQueue, thread);
    } else {
        KASSERT0(0,
                 "Is_Thread_On_Run_Queue unimplemented for non-RR schedulers\n");
    }
}
