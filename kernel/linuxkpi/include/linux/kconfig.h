#pragma once
/* The Linux-shaped layer (kernel/linuxkpi) that NVIDIA's open driver compiles against, in place of Linux (see
 * docs/nvidia/plan.md). This header comes first in every file, as Linux's does: which kernel "options" the layer
 * stands for. Only what the layer really provides is switched on; the glue checks these and leaves out the rest. */

#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_64BIT 1
#define CONFIG_MMU 1
#define CONFIG_SMP 1
#define CONFIG_PCI 1
#define CONFIG_PCI_MSI 1
#define CONFIG_ACPI 1
#define CONFIG_X86_PAT 1
#define CONFIG_NR_CPUS 256
#define CONFIG_PAGE_OFFSET 0
#define CONFIG_HZ 1000

/* IS_ENABLED(CONFIG_FOO): 1 if the option is set to 1 (as Linux's, by token pasting) */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_BUILTIN(option) __is_defined(option)
#define IS_MODULE(option) __is_defined(option##_MODULE)
#define IS_ENABLED(option) (IS_BUILTIN(option) || IS_MODULE(option))
#define IS_REACHABLE(option) IS_ENABLED(option)
