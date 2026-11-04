#!/usr/bin/env python3
"""The index of a newc cpio archive (the base system, build/base.cpio): where each file's bytes are in it.

The kernel makes the base system's files from this at boot, at once, and reads each file's bytes from the
medium when it is first used, instead of unpacking the whole archive (kernel/fs/cpio.c, initramfs_from_index,
which describes the format). It travels after the placeholder line the bootloader loads ("OS-LAZY PATH SIZE").

    cpio-index.py ARCHIVE INDEX
"""
import struct
import sys

S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0o170000, 0o100000, 0o040000, 0o120000


def entries(f):
    """(header offset, ino, mode, nlink, devmajor, devminor, size, data offset, name) for each member."""
    off = 0
    while True:
        f.seek(off)
        h = f.read(110)
        if len(h) < 110:
            sys.exit('cpio-index: the archive ends without a trailer')
        if h[:6] not in (b'070701', b'070702'):
            sys.exit('cpio-index: no cpio header at %d' % off)
        field = lambda i: int(h[6 + 8 * i:14 + 8 * i], 16)
        ino, mode, nlink, size = field(0), field(1), field(4), field(6)
        devmajor, devminor, namesize = field(7), field(8), field(11)
        name = f.read(namesize)[:-1]
        data = (off + 110 + namesize + 3) & ~3
        yield off, ino, mode, nlink, devmajor, devminor, size, data, name
        if name == b'TRAILER!!!':
            return
        off = (data + size + 3) & ~3


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    archive, out = sys.argv[1], sys.argv[2]
    with open(archive, 'rb') as f:
        f.seek(0, 2)
        total = f.tell()
        members = list(entries(f))
        trailer = members[-1][0]
        members = members[:-1]

        # Hard links: every name of a file has its inode number; GNU cpio puts the bytes with one of them (the
        # last). Each name gets the bytes' place, and the names of one file a group number of their own.
        carrier = {}
        for m in members:
            _, ino, mode, nlink, dmaj, dmin, size, data, _ = m
            if mode & S_IFMT == S_IFREG and nlink > 1:
                key = (dmaj, dmin, ino)
                if size or key not in carrier:
                    carrier[key] = (size, data)
        groups = {}

        body = bytearray()
        count = 0
        for _, ino, mode, nlink, dmaj, dmin, size, data, name in members:
            while name.startswith(b'./'):
                name = name[2:]
            if not name or name == b'.':
                continue
            kind = mode & S_IFMT
            group, target = 0, b''
            if kind == S_IFREG:
                if nlink > 1:
                    key = (dmaj, dmin, ino)
                    group = groups.setdefault(key, len(groups) + 1)
                    size, data = carrier[key]
            elif kind == S_IFLNK:
                f.seek(data)
                target = f.read(size)
                size, data = len(target), 0
            elif kind == S_IFDIR:
                size, data = 0, 0
            else:
                continue  # (devices, fifos: none in the base system, and nothing the kernel makes from it)
            if len(name) > 400 or len(target) > 500:
                sys.exit('cpio-index: name too long: %r' % name)
            e = struct.pack('<IIQQHHI', mode, group, size, data, len(name), len(target), 0) + name + target
            body += e + b'\0' * (-len(e) % 4)
            count += 1

    with open(out, 'wb') as o:
        o.write(b'OSIDX1\n\0' + struct.pack('<QQII', total, trailer, count, 0))
        o.write(body)
    print('cpio-index: %d entries, %d KiB, for %d MiB' % (count, (len(body) + 32) >> 10, total >> 20))


if __name__ == '__main__':
    main()
