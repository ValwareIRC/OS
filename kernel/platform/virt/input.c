#include <kernel/drivers.h>
#include <kernel/platform.h>

void platform_input_init(void) { virtio_input_init(); }
