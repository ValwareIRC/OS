/* void switch_context(uint64_t *save_rsp, uint64_t new_rsp)
 * Saves callee-saved registers on the current stack, stores the stack pointer
 * into *save_rsp, then resumes the thread whose stack is new_rsp. */
.text
.global switch_context
switch_context:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

/* First "return" of a new thread lands here (see thread_create). */
.global thread_start_trampoline
thread_start_trampoline:
    call thread_start
    ud2

.section .note.GNU-stack, "", @progbits
