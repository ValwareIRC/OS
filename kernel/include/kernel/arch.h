#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x1b /* 0x18 | RPL3; data before code for SYSRET */
#define GDT_USER_CODE   0x23 /* 0x20 | RPL3 */
#define GDT_TSS         0x28

#define IST_DOUBLE_FAULT 1

/* Interrupt vectors. 0-31 are CPU exceptions. */
#define VEC_TIMER    0x20
#define VEC_RESCHED  0x21
#define VEC_TLB      0x22
#define VEC_SPURIOUS 0xff

/* Register frame pushed by isr.S, in push order reversed. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

typedef void (*irq_handler_t)(struct regs *r);

struct thread;

/* Per-CPU state, reached through the GS base register. `self` must stay
 * first: this_cpu() reads it with a single gs-relative load. */
struct cpu {
    struct cpu *self;     /* offset 0 */
    uint64_t kernel_rsp;  /* offset 8: stack top for syscall entry */
    uint64_t user_rsp;    /* offset 16: scratch for syscall entry */
    uint32_t id;       /* 0..cpu_count-1, BSP is 0 */
    uint32_t lapic_id;
    struct thread *current;
    struct thread *idle;
    struct thread *prev; /* thread we just switched away from */
    uint64_t ticks;
    volatile bool online;
    uint64_t gdt[7];
    struct tss tss;
    uint8_t *df_stack;
    struct mm *active_mm;          /* user page tables loaded in CR3, or NULL */
    volatile int tlb_flush_pending; /* set by tlb_shootdown, cleared by the flush */
};

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#define MAX_CPUS 256
extern struct cpu *cpus[MAX_CPUS];
extern uint32_t cpu_count;

void gdt_init(struct cpu *c);
void idt_init(void);
void idt_load(void);
void idt_set_handler(uint8_t vector, irq_handler_t handler);
void cpu_set_percpu(struct cpu *c);
void cpu_enable_features(void);

/* ACPI */
void acpi_init(void);
void *acpi_find_table(const char *sig, int index);

/* APIC: local APIC on every CPU, I/O APIC for device interrupts. */
void apic_init(void);       /* BSP: parse MADT, disable 8259, calibrate timer */
void lapic_init_cpu(void);  /* every CPU: enable local APIC + periodic timer */
void lapic_eoi(void);
uint32_t lapic_id(void);
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic, bool level, bool active_low);
uint32_t irq_to_gsi(uint8_t isa_irq, bool *level, bool *active_low);
extern volatile uint64_t global_ticks;
#define HZ 250

/* SMP */
void smp_init(void);
