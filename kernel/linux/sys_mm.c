#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

SYSCALL(brk) {
    UNUSED_ARGS();
    struct mm *mm = proc_current()->mm;
    uint64_t want = a1;
    if (want < mm->brk_start || want > mm->brk_start + (1ULL << 40))
        return (int64_t)mm->brk;
    uint64_t old_end = ALIGN_UP(mm->brk, PAGE_SIZE), new_end = ALIGN_UP(want, PAGE_SIZE);
    if (new_end > old_end) {
        if (mm_find_vma(mm, old_end) || mm_map(mm, old_end, new_end, PROT_READ | PROT_WRITE, VMA_ANON) < 0)
            return (int64_t)mm->brk;
    } else if (new_end < old_end) {
        mm_unmap(mm, new_end, old_end);
    }
    mm->brk = want;
    return (int64_t)want;
}

SYSCALL(mmap) {
    UNUSED_ARGS();
    uint64_t addr = a1, len = a2, prot = a3, flags = a4, off = a6;
    int fd = (int)a5;
    if (!len || (off & 0xfff) || len > USER_TOP)
        return -EINVAL;
    if (!(flags & (MAP_PRIVATE | MAP_SHARED)))
        return -EINVAL;
    bool shared = flags & MAP_SHARED; /* also MAP_SHARED_VALIDATE */
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    struct mm *mm = proc_current()->mm;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (fixed && ((addr & 0xfff) || addr > USER_TOP - len))
        return -EINVAL;
    if (flags & MAP_FIXED_NOREPLACE) {
        for (uint64_t a = addr; a < addr + len; a += PAGE_SIZE)
            if (mm_find_vma(mm, a))
                return -EEXIST;
    }

    struct file *f = NULL;
    struct pageobj *obj = NULL;
    struct vma_name *name = NULL; /* for /proc/<pid>/maps */
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (!f)
            return -EBADF;
        if (f->vnode->type == VN_REG)
            name = vma_name_for_file(f);
        if (f->vnode->ops->mmap_phys) {
            uint64_t phys;
            int r = f->vnode->ops->mmap_phys(f->vnode, off, len, &phys);
            if (r == 0 && !fixed)
                addr = mm_map_free(mm, len, addr, (uint32_t)prot, VMA_PHYS); /* reserve, then fill below */
            if (r == 0 && !addr)
                r = -ENOMEM;
            if (r == 0)
                r = mm_map_phys(mm, addr, addr + len, (uint32_t)prot, phys);
            file_put(f);
            return r < 0 ? r : (int64_t)addr;
        }
        if (f->vnode->type != VN_REG) {
            file_put(f);
            return -ENODEV;
        }
        if (shared && f->vnode->ops->mmap_shared) {
            /* The pages are the file's own, so writing through them is
             * writing the file: that needs a descriptor open for writing. */
            int r = 0;
            if ((prot & PROT_WRITE) && (f->flags & O_ACCMODE) != O_RDWR)
                r = -EACCES;
            else
                r = f->vnode->ops->mmap_shared(f->vnode, (uint32_t)prot, &obj);
            file_put(f);
            if (r < 0) {
                if (name)
                    vma_name_put(name);
                return r;
            }
            f = NULL;
        }
    } else if (shared) {
        /* Shared anonymous memory: an object of its own, so fork shares it. */
        obj = pageobj_create();
        if (!obj || pageobj_truncate(obj, len) < 0) {
            pageobj_put(obj);
            return -ENOMEM;
        }
        off = 0;
    }
    if (obj) {
        int64_t r = mm_map_shared(mm, addr, len, (uint32_t)prot, fixed, obj, off / PAGE_SIZE);
        pageobj_put(obj);
        if (name) {
            if (r >= 0)
                mm_set_name(mm, (uint64_t)r, (uint64_t)r + len, name, off / PAGE_SIZE);
            vma_name_put(name);
        }
        return r;
    }

    /* Mapped writable at first so file contents can be copied in, then
     * narrowed to the requested protection. */
    uint32_t map_prot = (uint32_t)prot | (f ? PROT_WRITE : 0), map_flags = f ? 0 : VMA_ANON;
    int r = 0;
    if (fixed) {
        r = mm_map(mm, addr, addr + len, map_prot, map_flags);
    } else {
        addr = mm_map_free(mm, len, addr, map_prot, map_flags);
        if (!addr) {
            if (f)
                file_put(f);
            if (name)
                vma_name_put(name);
            return -ENOMEM;
        }
    }
    if (r == 0 && name)
        mm_set_name(mm, addr, addr + len, name, off / PAGE_SIZE);
    if (name)
        vma_name_put(name);
    if (r == 0 && f) {
        /* Private file mappings are copies, made now: later writes to the
         * file are not seen (Linux would show them until the page is first
         * written). Read in chunks straight into the new pages: big ones from
         * a file big enough to need them (a disk read per chunk is the cost),
         * taken from the page allocator, not the heap, which would keep the
         * arena it grew for it for good. */
        uint64_t size = f->vnode->size;
        size_t pages = len >= (1u << 20) && size - (off < size ? off : size) >= (1u << 20) ? 128 : 16;
        uint64_t phys = pmm_alloc(pages);
        while (!phys && pages > 1) /* memory in small pieces: a smaller buffer, more reads */
            phys = pmm_alloc(pages /= 4);
        uint8_t *kb = phys ? phys_to_virt(phys) : NULL;
        size_t chunk = pages * PAGE_SIZE;
        if (!kb)
            r = -ENOMEM;
        for (uint64_t done = 0; r == 0 && off + done < size && done < len;) {
            uint64_t want = len - done < chunk ? len - done : chunk;
            int64_t n = file_pread(f, kb, want, off + done);
            if (n <= 0)
                break;
            r = mm_copy_out(mm, addr + done, kb, (size_t)n);
            done += (uint64_t)n;
        }
        if (phys)
            pmm_free(phys, pages);
        if (r == 0 && !(prot & PROT_WRITE))
            r = mm_protect(mm, addr, addr + len, (uint32_t)prot);
    }
    if (f)
        file_put(f);
    if (r < 0) {
        mm_unmap(mm, addr, addr + len);
        return r;
    }
    return (int64_t)addr;
}

SYSCALL(munmap) {
    UNUSED_ARGS();
    if (a1 & 0xfff)
        return -EINVAL;
    return mm_unmap(proc_current()->mm, a1, a1 + a2);
}

SYSCALL(mprotect) {
    UNUSED_ARGS();
    if (a1 & 0xfff)
        return -EINVAL;
    return mm_protect(proc_current()->mm, a1, a1 + a2, (uint32_t)a3);
}

SYSCALL(madvise) {
    UNUSED_ARGS();
    /* Allocators and JITs rely on DONTNEED reading back zeros; every other
     * piece of advice is a hint we may ignore. */
    if (a3 == MADV_DONTNEED) {
        if (a2 > USER_TOP)
            return -EINVAL;
        return mm_dontneed(proc_current()->mm, a1, a1 + a2);
    }
    return 0;
}

SYSCALL(msync) {
    UNUSED_ARGS();
    return 0; /* shared mappings are the file's own pages: nothing to write back */
}

SYSCALL(mremap) {
    UNUSED_ARGS();
    uint64_t flags = a4;
    if (flags & ~(uint64_t)(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        return -EINVAL;
    /* Moving to a caller-chosen address or keeping the old range are not
     * supported; ENOMEM sends libc down its mmap + copy path. */
    if (flags & (MREMAP_FIXED | MREMAP_DONTUNMAP))
        return -ENOMEM;
    return mm_remap(proc_current()->mm, a1, a2, a3, flags & MREMAP_MAYMOVE);
}
