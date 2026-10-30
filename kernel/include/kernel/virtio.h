#pragma once
#include <asm/io.h>
#include <kernel/lib.h>
#include <stdint.h>

/* What the legacy and the modern virtio transports share: the device status
 * bits and the split virtqueue layout. */

#define VIRTIO_VENDOR 0x1af4

#define STATUS_ACK         1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8
#define STATUS_FAILED      0x80

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct vring_avail {
    uint16_t flags;
    volatile uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem {
    uint32_t id, len;
};

struct vring_used {
    volatile uint16_t flags;
    volatile uint16_t idx;
    struct vring_used_elem ring[];
};

struct virtq {
    uint16_t size, avail_idx, used_seen;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
};

static inline size_t virtq_used_offset(uint16_t size, size_t used_align) {
    return ALIGN_UP(16u * size + 6 + 2u * size, used_align);
}

/* The bytes a queue of `size` entries takes, its used ring aligned to `used_align`. */
static inline size_t virtq_bytes(uint16_t size, size_t used_align) {
    return virtq_used_offset(size, used_align) + 6 + 8u * size;
}

/* Lays out an empty queue of `size` entries at `mem`. */
static inline void virtq_carve(struct virtq *q, void *mem, uint16_t size, size_t used_align) {
    uint8_t *p = mem;
    memset(p, 0, virtq_bytes(size, used_align));
    *q = (struct virtq){size, 0, 0, (struct vring_desc *)p, (struct vring_avail *)(p + 16u * size),
                        (struct vring_used *)(p + virtq_used_offset(size, used_align))};
}

/* Returns the head of the next chain the device has finished with, or -1 when there is none. */
static inline int virtq_next_used(struct virtq *q) {
    if (q->used->idx == q->used_seen)
        return -1;
    dma_rmb();
    return (int)(q->used->ring[q->used_seen++ % q->size].id % q->size);
}
