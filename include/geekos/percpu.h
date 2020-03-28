/* This file is a placeholder for storing per-cpu variables in a segment that is not 
   saved and restored per thread, but rather left alone although different on a per-cpu
   basis. */

#include <geekos/smp.h>

struct PerCPU_Var {
    int cpuId;
    struct Kernel_Thread *currentThread;
};

struct PerCPU_Var g_PerCPU_Vars[8];

void Init_PerCPU(int cpu);
int PerCPU_Get_CPU(void);
struct Kernel_Thread *PerCPU_Get_Current(void);
