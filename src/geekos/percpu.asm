	;;  percpu.asm redefines current_thread macros
    ;;  to use the per-cpu segment.

;; eax = *( &g_currentThreads[Get_CPU_ID()] )
%macro Get_Current_Thread_To_EAX 0
    mov	eax, [gs:4]
%endmacro
%macro Set_Current_Thread_From_EBX 0
    mov	[gs:4], ebx
%endmacro
%macro Push_Current_Thread_PTR 0
    push dword [gs:4]
%endmacro