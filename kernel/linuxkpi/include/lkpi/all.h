#pragma once
/* kernel/linuxkpi: everything the layer has. Every one of Linux's small headers NVIDIA's glue includes (linux/*.h,
 * asm/*.h...) includes this, so a file sees the same whichever it names. */
#include <lkpi/core.h>
#include <lkpi/sched.h>
#include <lkpi/mm.h>
