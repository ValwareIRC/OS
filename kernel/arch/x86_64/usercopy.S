/* Kernel accesses to user memory. Each instruction that touches user memory
 * has an entry in __ex_table (instruction address, fixup address): when it
 * faults and the page fault handler cannot resolve the fault, it resumes at
 * the fixup instead of panicking, and the routine returns -EFAULT. These are
 * leaf routines that never move rsp, so one shared fixup can just return. */

#define EFAULT 14

.macro EX insn, fixup
    .pushsection __ex_table, "a"
    .balign 8
    .quad \insn, \fixup
    .popsection
.endm

.text

/* int __copy_user(void *dst, const void *src, size_t len): 0 or -EFAULT */
.global __copy_user
__copy_user:
    movq %rdx, %rcx
1:  rep movsb
    xorl %eax, %eax
    ret
    EX 1b, .Lfault

/* int64_t __strncpy_user(char *dst, const char *src, size_t size): length of
 * the string (the NUL is copied), `size` if there is no NUL within size
 * bytes, or -EFAULT. */
.global __strncpy_user
__strncpy_user:
    xorl %eax, %eax
2:  cmpq %rdx, %rax
    jae 4f
3:  movb (%rsi,%rax), %cl
    movb %cl, (%rdi,%rax)
    testb %cl, %cl
    jz 4f
    incq %rax
    jmp 2b
4:  ret
    EX 3b, .Lfault

/* int __cmpxchg_user32(uint64_t uaddr, uint32_t expected, uint32_t desired,
 *                      uint32_t *cur): *cur = value found; 0 or -EFAULT */
.global __cmpxchg_user32
__cmpxchg_user32:
    movl %esi, %eax
5:  lock cmpxchgl %edx, (%rdi)
    movl %eax, (%rcx)
    xorl %eax, %eax
    ret
    EX 5b, .Lfault

.Lfault:
    movq $-EFAULT, %rax
    ret

.section .note.GNU-stack, "", @progbits
