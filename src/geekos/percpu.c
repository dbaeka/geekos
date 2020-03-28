#include <geekos/percpu.h>
#include <geekos/kthread.h>


void Init_PerCPU(int cpu) {
    g_PerCPU_Vars[cpu].cpuId = cpu;
}

int PerCPU_Get_CPU(void) {
    int ret;
    __asm__ __volatile__(
    "movl %%gs:0, %0"
    :"=r"(ret)
    );
    return ret;
}

struct Kernel_Thread *PerCPU_Get_Current(void) {
    struct Kernel_Thread *ret;
    __asm__ __volatile__(
    "movl %%gs:4, %0"
    :"=r"(ret)
    );
    return ret;
}
