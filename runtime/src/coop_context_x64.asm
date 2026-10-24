; Cooperative guest-thread context switch (Windows x64), used by coop_scheduler.cpp in place of Windows fibers.
;
; A parked context is entirely on its own stack: the callee-saved registers, XMM6-15, MXCSR, the x87 control
; word and the thread block's stack bounds are pushed there, and the context is the resulting stack pointer.
; Copying a parked stack's bytes and its stack pointer is therefore a snapshot of that thread (rollback).
;
; Frame, from the saved stack pointer up (256 bytes, then the return address):
;   +00h XMM6..XMM15 (10 x 16)   +0A0h MXCSR   +0A4h x87 control word
;   +0A8h TEB DeallocationStack  +0B0h TEB StackLimit  +0B8h TEB StackBase
;   +0C0h rbp  +0C8h rbx  +0D0h rdi  +0D8h rsi  +0E0h r12  +0E8h r13  +0F0h r14  +0F8h r15
;   +100h return address
; coop_context_make (coop_scheduler.cpp) lays out the same frame for a context that has not run yet, returning
; into coop_context_trampoline with the entry in r12 and its argument in r13.

.code

; void coop_context_switch(void** save_sp /* rcx */, void* load_sp /* rdx */)
coop_context_switch PROC
    push r15
    push r14
    push r13
    push r12
    push rsi
    push rdi
    push rbx
    push rbp
    mov rax, gs:[08h]               ; StackBase
    push rax
    mov rax, gs:[10h]               ; StackLimit
    push rax
    mov rax, gs:[1478h]             ; DeallocationStack
    push rax
    sub rsp, 0A8h
    movups [rsp+000h], xmm6
    movups [rsp+010h], xmm7
    movups [rsp+020h], xmm8
    movups [rsp+030h], xmm9
    movups [rsp+040h], xmm10
    movups [rsp+050h], xmm11
    movups [rsp+060h], xmm12
    movups [rsp+070h], xmm13
    movups [rsp+080h], xmm14
    movups [rsp+090h], xmm15
    stmxcsr dword ptr [rsp+0A0h]
    fnstcw word ptr [rsp+0A4h]

    mov [rcx], rsp
    mov rsp, rdx

    movups xmm6, [rsp+000h]
    movups xmm7, [rsp+010h]
    movups xmm8, [rsp+020h]
    movups xmm9, [rsp+030h]
    movups xmm10, [rsp+040h]
    movups xmm11, [rsp+050h]
    movups xmm12, [rsp+060h]
    movups xmm13, [rsp+070h]
    movups xmm14, [rsp+080h]
    movups xmm15, [rsp+090h]
    ldmxcsr dword ptr [rsp+0A0h]
    fldcw word ptr [rsp+0A4h]
    add rsp, 0A8h
    pop rax
    mov gs:[1478h], rax
    pop rax
    mov gs:[10h], rax
    pop rax
    mov gs:[08h], rax
    pop rbp
    pop rbx
    pop rdi
    pop rsi
    pop r12
    pop r13
    pop r14
    pop r15
    ret
coop_context_switch ENDP

; First return of a new context: entry(argument) on an aligned stack with shadow space. The entry never returns.
coop_context_trampoline PROC
    mov rcx, r13
    and rsp, -16
    sub rsp, 20h
    call r12
    int 3
coop_context_trampoline ENDP

END
