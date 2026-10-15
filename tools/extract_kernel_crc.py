#!/usr/bin/env python3
"""extract_kernel_crc.py — 从 ARM64 内核 Image 提取全量导出符号 CRC 表 (__kcrctab)

用途
----
设备内核开启 CONFIG_MODVERSIONS + CONFIG_EXTENDED_MODVERSIONS=y 时, insmod 会逐
符号比对 CRC (且**只查 __version_ext_crcs**)。自编译 .ko 的 CRC 来自 vanilla GKI,
与厂商改动内核不符 → "disagrees about version of symbol xxx"。
靠设备 .ko 的 __versions 拼映射表覆盖率不足; 本工具直接读内核镜像里的权威
__kcrctab 全表 (约 9.9k 导出符号), 一次覆盖。

定位原理 (全部经实测闭环验证, 见 docs/KO_CRC_PIPELINE.md)
--------------------------------------------------------
 1. ARM64 Image: 文件偏移 0x38 魔数为 b'ARM\\x64'; kernel 从文件头开始扁平映射。
 2. __ksymtab / __ksymtab_gpl: 12 字节条目
        struct kernel_symbol { int value_offset, name_offset, namespace_offset; }
    名字文件偏移 = 条目偏移 + 4 + name_offset。
    * 条目按符号名字母序排列 (SORT(___ksymtab+*))
    * 但 name 目标按目标文件顺序放置 → **name 目标不单调**, 扫描时不可要求递增
    * 段真起点 = 字母序首个导出符号 (实测自 I_BDEV 开始)
 3. __kcrctab / __kcrctab_gpl: 与 ksymtab **同序**的 u32 数组。
    基址用「设备模块真实 CRC 出现位置」投票反解:
        base = pos(crc_of_sym) - idx(sym)*4   → 取众数
    实测票数压倒性 (327 vs 50), 且 0 个符号冲突。

用法
----
  python extract_kernel_crc.py <kernel_image> <out.json> [device.ko ...]
    kernel_image : ARM64 Image (可直接用 boot.img 的 kernel 段)
    device.ko    : 可选; 用于投票求解基址与交叉校验 (强烈建议提供)
"""
import json
import re
import struct
import sys

SYM_RE = re.compile(rb"^[A-Za-z_][A-Za-z0-9_]*$")
ENTRY = 12


def load(path):
    with open(path, "rb") as f:
        return f.read()


def name_at(d, off):
    if off < 0 or off >= len(d):
        return None
    c = d[off]
    if not (65 <= c <= 90 or 97 <= c <= 122 or c == 95):
        return None
    q = d.find(b"\0", off)
    if q < 0 or q == off or q - off > 96:
        return None
    s = d[off:q]
    return s.decode() if SYM_RE.match(s) else None


def entry_name(d, off):
    noff = struct.unpack_from("<i", d, off + 4)[0]
    return name_at(d, off + 4 + noff)


def seg_start(d, anywhere):
    """从 anywhere(任一已知合法条目) 沿 12 字节网格回溯到段首"""
    s = anywhere
    while s - ENTRY >= 0 and entry_name(d, s - ENTRY):
        s -= ENTRY
    n = 0
    while s + n * ENTRY + ENTRY <= len(d) and entry_name(d, s + n * ENTRY):
        n += 1
    return s, n


def scan_segments(d, lo, hi, need=48):
    """粗扫找所有「连续 >= need 条合法」的起点"""
    out = []
    for base in range(lo, hi, 4):
        ok = True
        for i in range(need):
            off = base + i * ENTRY
            if off + ENTRY > len(d) or entry_name(d, off) is None:
                ok = False
                break
        if ok:
            out.append(base)
    return out


def read_ko_crcs(path):
    d = load(path)
    if d[:4] != b"\x7fELF" or d[4] != 2:
        return {}
    endian = "<" if d[5] == 1 else ">"
    e_shoff = struct.unpack_from(endian + "Q", d, 0x28)[0]
    es = struct.unpack_from(endian + "H", d, 0x3A)[0]
    en = struct.unpack_from(endian + "H", d, 0x3C)[0]
    eix = struct.unpack_from(endian + "H", d, 0x3E)[0]
    h = e_shoff + eix * es
    so = struct.unpack_from(endian + "Q", d, h + 0x18)[0]
    ss = struct.unpack_from(endian + "Q", d, h + 0x20)[0]
    shstr = d[so:so + ss]
    secs = {}
    for i in range(en):
        off = e_shoff + i * es
        no = struct.unpack_from(endian + "I", d, off)[0]
        nm = shstr[no:shstr.find(b"\0", no)].decode("ascii", "replace")
        secs[nm] = (struct.unpack_from(endian + "Q", d, off + 0x18)[0],
                    struct.unpack_from(endian + "Q", d, off + 0x20)[0])
    if "__version_ext_crcs" in secs:
        co, cs = secs["__version_ext_crcs"]
        no, ns = secs["__version_ext_names"]
        names = [s.decode("ascii", "replace") for s in d[no:no + ns].split(b"\0") if s][:cs // 4]
        return {nm: struct.unpack_from("<I", d, co + i * 4)[0] for i, nm in enumerate(names)}
    if "__versions" in secs:
        off, size = secs["__versions"]
        return {d[e + 8:e + 8 + 56].split(b"\0")[0].decode("ascii", "replace"):
                struct.unpack_from("<I", d, e)[0]
                for e in (off + i * 64 for i in range(size // 64))}
    return {}


def vote_base(d, symtab, ko_crcs, lo, hi):
    from collections import Counter, defaultdict
    idx_of = {s: i for s, i, _ in symtab}
    want = set(ko_crcs.values())
    pos_of = defaultdict(list)
    for off in range(lo, hi, 4):
        v = struct.unpack_from("<I", d, off)[0]
        if v in want:
            pos_of[v].append(off)
    votes = Counter()
    for s, crc in ko_crcs.items():
        if s in idx_of:
            for p in pos_of.get(crc, ()):
                votes[p - idx_of[s] * 4] += 1
    return votes


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    image, out = sys.argv[1], sys.argv[2]
    kos = sys.argv[3:]
    d = load(image)
    if d[0x38:0x3C] != b"ARM\x64":
        raise SystemExit("不是 ARM64 Image (偏移 0x38 魔数不符)")
    print(f"image {len(d)} (0x{len(d):x})")

    merged = {}
    for ko in kos:
        merged.update(read_ko_crcs(ko))
    print(f"设备模块 CRC {len(merged)} 条")

    # 段扫描范围: .rodata 后段 (ksymtab 位于其中)
    lo, hi = len(d) // 2, len(d) - 0x10000
    cands = scan_segments(d, lo, hi)
    print(f"候选起点 {len(cands)} 个")
    starts = {}
    for c in cands:
        s, n = seg_start(d, c)
        if n >= 1000:
            starts[s] = n
    order = sorted(starts.items())
    print("连续段:")
    for s, n in order:
        print(f"  起点 0x{s:x}  条目 {n}  首 {entry_name(d, s)!r}  尾 {entry_name(d, s + (n-1)*ENTRY)!r}")

    if len(order) < 2:
        raise SystemExit("未能同时定位 __ksymtab 与 __ksymtab_gpl")

    (nstart, nlen), (gstart, glen) = order[0], order[1]
    non = [(entry_name(d, nstart + i * ENTRY), i, nstart + i * ENTRY) for i in range(nlen)]
    gpl = [(entry_name(d, gstart + i * ENTRY), i, gstart + i * ENTRY) for i in range(glen)]

    vn = vote_base(d, non, merged, nstart, len(d))
    vg = vote_base(d, gpl, merged, nstart, len(d))
    if not vn or not vg:
        raise SystemExit("投票失败: 请提供至少一个设备厂商 .ko 作为 CRC 来源")
    kb = vn.most_common(1)[0][0]
    kbg = vg.most_common(1)[0][0]
    print(f"\n__kcrctab     base=0x{kb:x}  (票 {vn[kb]}, 次 {vn.most_common(2)[-1][1] if len(vn)>1 else 0})")
    print(f"__kcrctab_gpl base=0x{kbg:x}  (票 {vg[kbg]})")

    table = {s: struct.unpack_from("<I", d, kb + i * 4)[0] for s, i, _ in non}
    table.update({s: struct.unpack_from("<I", d, kbg + i * 4)[0] for s, i, _ in gpl})
    print(f"完整表 {len(table)} 个导出符号")

    for ko in kos:
        kc = read_ko_crcs(ko)
        hit = sum(1 for s, c in kc.items() if table.get(s) == c)
        wrong = [s for s, c in kc.items() if s in table and table[s] != c]
        miss = [s for s in kc if s not in table]
        nm = ko.replace("\\", "/").split("/")[-1]
        print(f"  {nm:28s} 一致 {hit:3d} / 冲突 {len(wrong):3d} / 未覆盖 {len(miss):3d} (共 {len(kc)})")
        for s in wrong[:5]:
            print(f"      !! 冲突 {s}")
        if not wrong and hit:
            print(f"      => 校验通过 (0 冲突)")

    with open(out, "w") as f:
        json.dump(table, f, indent=0, sort_keys=True)
    print(f"写出 {out}")


if __name__ == "__main__":
    main()
