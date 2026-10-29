/* SYSCALL entry. On entry: rcx = user rip, r11 = user rflags, rsp = user
 * stack, interrupts masked by FMASK, GS = user's. We switch to the thread's
 * kernel stack and build the same struct regs frame interrupts use, so
 * fork, exec and signal delivery handle both paths identically. We always
 * return with iretq (via return_to_user_common): it is slower than sysret
 * but immune to the non-canonical-RIP sysret problem. */

#define GDT_USER_DATA 0x1b
#define GDT_USER_CODE 0x23

.text
.global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:16          /* cpu->user_rsp */
    movq %gs:8, %rsp           /* cpu->kernel_rsp */
    pushq $GDT_USER_DATA       /* ss */
    pushq %gs:16               /* rsp */
    pushq %r11                 /* rflags */
    pushq $GDT_USER_CODE       /* cs */
    pushq %rcx                 /* rip */
    pushq $0                   /* error */
    pushq $0x80                /* vector: marks a syscall frame */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    sti
    movq %rsp, %rdi
    call syscall_dispatch
    jmp return_to_user_common

.section .note.GNU-stack, "", @progbits
