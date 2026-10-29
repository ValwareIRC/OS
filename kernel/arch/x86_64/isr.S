/* Interrupt entry stubs. Every vector pushes a uniform frame (error code,
 * vector number, general registers) matching struct regs, then calls
 * isr_dispatch(struct regs *). */

.altmacro

.macro ISR_NOERR n
isr_\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
isr_\n:
    pushq $\n
    jmp isr_common
.endm

.text
.p2align 4
isr_common:
    cld
    /* Coming from user mode: switch GS to the per-CPU block. The saved CS
     * sits above the vector and error code at this point. */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
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
    movq %rsp, %rdi
    call isr_dispatch
    jmp return_to_user_common

/* void return_to_user(struct regs *frame): restores a full register frame
 * and returns with iretq; also the shared exit path of isr_common and
 * syscall_entry. Swaps GS back when returning to user mode. */
.global return_to_user
return_to_user:
    movq %rdi, %rsp
.global return_to_user_common
return_to_user_common:
    cli
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    testb $3, 8(%rsp)
    jz 2f
    swapgs
2:
    iretq

/* Vectors that push a CPU error code: 8, 10-14, 17, 21, 29, 30. */
.set i, 0
.rept 256
    .if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
        ISR_ERR %i
    .else
        ISR_NOERR %i
    .endif
    .set i, i + 1
.endr

.macro ISR_ADDR n
    .quad isr_\n
.endm

.section .rodata
.p2align 3
.global isr_table
isr_table:
.set i, 0
.rept 256
    ISR_ADDR %i
    .set i, i + 1
.endr

.section .note.GNU-stack, "", @progbits
