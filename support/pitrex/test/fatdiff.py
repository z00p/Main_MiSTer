#!/usr/bin/env python3
"""Compare two FAT32 card images byte for byte, apart from what differs between two builds of the same card by design:
the volume ID mkfs.vfat takes from the clock, and the times in directory entries (folders get the time they were made).
Everything else has to match: boot sectors, FSInfo, both FATs, every cluster of the data area (directories with their
times masked). Usage: fatdiff.py a.img b.img   (exit 0 = same, 1 = different, 2 = not FAT32 / not the same geometry)"""
import struct
import sys


def geometry(img):
    with open(img, "rb") as f:
        bs = f.read(512)
    bps, spc, rsv, nfat = struct.unpack_from("<HBHB", bs, 11)
    fatsz = struct.unpack_from("<I", bs, 36)[0]
    total = struct.unpack_from("<I", bs, 32)[0] or struct.unpack_from("<H", bs, 19)[0]
    root = struct.unpack_from("<I", bs, 44)[0]
    if bs[82:90] != b"FAT32   " or not bps or not spc:
        return None
    return dict(bps=bps, spc=spc, rsv=rsv, nfat=nfat, fatsz=fatsz, total=total, root=root)


def mask_boot(sec):
    b = bytearray(sec)
    b[67:71] = b"\0\0\0\0"            # BS_VolID
    return bytes(b)


def mask_dir(data):
    b = bytearray(data)
    for i in range(0, len(b), 32):
        if b[i] == 0:
            continue
        if b[i + 11] & 0x3F == 0x0F:  # long-name entry: no times
            continue
        b[i + 13:i + 20] = b"\0" * 7   # creation time (tenths, time, date), access date
        b[i + 22:i + 26] = b"\0" * 4   # write time and date
    return bytes(b)


def main(a, b):
    ga, gb = geometry(a), geometry(b)
    if not ga or ga != gb:
        print("not FAT32 or not the same geometry:", ga, gb)
        return 2
    g = ga
    fa, fb = open(a, "rb"), open(b, "rb")
    bps, spc = g["bps"], g["spc"]
    diffs = []

    # reserved area: boot sector and its backup (sector 6) with the volume ID masked, FSInfo and the rest as they are
    ra, rb = fa.read(g["rsv"] * bps), fb.read(g["rsv"] * bps)
    for s in range(g["rsv"]):
        x, y = ra[s * bps:(s + 1) * bps], rb[s * bps:(s + 1) * bps]
        if s in (0, 6):
            x, y = mask_boot(x), mask_boot(y)
        if x != y:
            diffs.append("reserved sector %d" % s)

    fat_bytes = g["fatsz"] * bps
    fat = fa.read(fat_bytes)
    if fat != fb.read(fat_bytes):
        diffs.append("FAT 1")
    for i in range(1, g["nfat"]):
        if fa.read(fat_bytes) != fb.read(fat_bytes):
            diffs.append("FAT %d" % (i + 1))

    nclus = (g["total"] - g["rsv"] - g["nfat"] * g["fatsz"]) // spc
    entries = struct.unpack_from("<%dI" % (len(fat) // 4), fat)

    def chain(c):
        out = []
        while 2 <= c < 0x0FFFFFF7 and len(out) <= nclus:
            out.append(c)
            c = entries[c] & 0x0FFFFFFF
        return out

    csize = bps * spc
    base = (g["rsv"] + g["nfat"] * g["fatsz"]) * bps

    def cluster(f, c):
        f.seek(base + (c - 2) * csize)
        return f.read(csize)

    # the directory clusters, from the root down (by image a's structure; image b's FAT is compared above)
    dirs, todo, seen = set(), [g["root"]], set()
    while todo:
        d = todo.pop()
        if d in seen:
            continue
        seen.add(d)
        for c in chain(d):
            dirs.add(c)
            data = cluster(fa, c)
            for i in range(0, csize, 32):
                e = data[i:i + 32]
                if e[0] in (0, 0xE5) or e[11] & 0x3F == 0x0F or not e[11] & 0x10 or e[0:2] == b". " or e[0:2] == b"..":
                    continue
                todo.append(struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0])

    for c in range(2, nclus + 2):
        x, y = cluster(fa, c), cluster(fb, c)
        if c in dirs:
            x, y = mask_dir(x), mask_dir(y)
        if x != y:
            diffs.append("cluster %d%s" % (c, " (directory)" if c in dirs else ""))
            if len(diffs) > 20:
                break
    if diffs:
        print("%s and %s differ: %s" % (a, b, ", ".join(diffs[:20])))
        return 1
    print("same: %s, %s (%d clusters, %d of them directories)" % (a.split("/")[-1], b.split("/")[-1], nclus, len(dirs)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
