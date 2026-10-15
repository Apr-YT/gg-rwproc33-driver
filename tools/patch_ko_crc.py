#!/usr/bin/env python3
"""patch_ko_crc.py — 用内核 CRC 表修补 .ko 的 MODVERSIONS 版本信息 (双写)

背景
----
设备内核 6.12.69 开启 CONFIG_EXTENDED_MODVERSIONS=y, check_version() **只查
__version_ext_crcs**; 旧 __versions 表仅为兼容保留。实测设备 KO 中两表
100% 同序同值 → 补丁必须双写, 只改 __versions 会被内核完全忽略。

CRC 来源优先级: 内核镜像 __kcrctab 全表 (extract_kernel_crc.py 产出, 约 9884 个
导出符号, 覆盖率远高于"用设备模块拼映射表")。

用法
----
  python patch_ko_crc.py <crc.json> <target.ko> [target.ko ...] [选项]
选项
  --vermagic=<字符串>   改写 vermagic。
                        ⚠️ 有 __versions 时内核走 same_magic(has_crcs=true):
                           **跳过首个空格前的内核版本, 只比对空格后的标志串**。
                        因此若只给版本号（不含空格）而原值含标志, 本工具会
                        **自动沿用原标志串**（" SMP preempt mod_unload modversions aarch64"）,
                        避免把标志一起截掉导致必然失败。
                        想要原样写入请加 --vermagic-exact。
  --vermagic-exact      原样写入 --vermagic, 不做标志串继承。
  --dry-run             只报告不写入

vermagic 取值从哪来
-------------------
最稳的是取**设备已加载厂商模块**的 vermagic（含标志），例如:
  adb pull /vendor/lib/modules/adsp_sleepmon.ko
  python patch_ko_crc.py --dump-modinfo adsp_sleepmon.ko
设备真实模块用的是 SDK 产出时的 release（如 6.12.69-android16-6-4k），
而 `uname -r` 含 KernelSU 的 git 描述（...-g586bfab1b9c5-abogki...）——
**两者都能过**（版本段被跳过），但标志串必须逐字一致。
"""
import json
import struct
import sys

MODULE_NAME_LEN = 56
VERS_ENTRY = 8 + MODULE_NAME_LEN


def elf_info(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not ELF")
    endian = "<" if data[5] == 1 else ">"
    e_shoff = struct.unpack_from(endian + "Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + "H", data, 0x3A)[0]
    e_shnum = struct.unpack_from(endian + "H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from(endian + "H", data, 0x3E)[0]
    hdr = e_shoff + e_shstrndx * e_shentsize
    shstr_off = struct.unpack_from(endian + "Q", data, hdr + 0x18)[0]
    shstr_size = struct.unpack_from(endian + "Q", data, hdr + 0x20)[0]
    shstr = data[shstr_off:shstr_off + shstr_size]
    secs = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off = struct.unpack_from(endian + "I", data, off)[0]
        nm = shstr[name_off:shstr.find(b"\0", name_off)].decode("ascii", "replace")
        secs[nm] = (struct.unpack_from(endian + "Q", data, off + 0x18)[0],
                    struct.unpack_from(endian + "Q", data, off + 0x20)[0])
    return secs, data


def read_tables(path):
    """-> (versions{name:(off,crc)}, ext{name:(off,crc)})"""
    secs, data = elf_info(path)
    v = {}
    if "__versions" in secs:
        off, size = secs["__versions"]
        for i in range(size // VERS_ENTRY):
            e = off + i * VERS_ENTRY
            v[data[e + 8:e + 8 + MODULE_NAME_LEN].split(b"\0")[0].decode("ascii", "replace")] = \
                (e, struct.unpack_from("<I", data, e)[0])
    x = {}
    if "__version_ext_crcs" in secs and "__version_ext_names" in secs:
        co, cs = secs["__version_ext_crcs"]
        no, ns = secs["__version_ext_names"]
        n = cs // 4
        names = [s.decode("ascii", "replace") for s in data[no:no + ns].split(b"\0") if s][:n]
        for i, nm in enumerate(names):
            x[nm] = (co + i * 4, struct.unpack_from("<I", data, co + i * 4)[0])
    return v, x, secs, data


def patch_vermagic(f, data, secs, value, exact, dry):
    """改写 .modinfo 的 vermagic 条目。

    容量按「值起点 → .modinfo 段尾」计算（vermagic 惯例是最后一条），
    这样即使此前被别的工具用 \\0 填充过也能正确恢复。
    若其后确有其它条目, 则收紧到原终止符处, 避免覆盖。
    """
    mi = secs.get(".modinfo")
    if not mi:
        print("  警告: 无 .modinfo 段, 跳过 vermagic")
        return
    off, size = mi
    blob = data[off:off + size]
    idx = blob.find(b"vermagic=")
    if idx < 0:
        print("  警告: .modinfo 内未找到 vermagic=")
        return
    vstart = idx + 9
    end = blob.find(b"\0", vstart)
    if end < 0:
        end = size
    old = blob[vstart:end].decode("ascii", "replace")

    final = value
    if not exact and " " not in value and " " in old:
        flags = old[old.index(" "):]          # " SMP preempt mod_unload modversions aarch64"
        final = value + flags
        print("  提示: --vermagic 未含标志串, 自动沿用原值标志 → %s" % final)

    cap = size - vstart                        # 可一直写到段尾
    tail = blob[end + 1:].strip(b"\0")
    if tail:
        cap = end - vstart                     # 后面还有条目 → 不能越界
        print("  注意: vermagic 后仍有 modinfo 条目, 容量收紧为 %d 字节" % cap)

    nb = final.encode()
    if len(nb) > cap:
        print("  警告: 新 vermagic 过长(%d>%d), 跳过" % (len(nb), cap))
        return
    if not dry:
        f.seek(off + vstart)
        f.write(nb + b"\0" * (cap - len(nb)))
    print("  vermagic: %s" % old)
    print("         -> %s" % final)
    print("         (字段容量 %d, 写入 %d, 填充 %d)" % (cap, len(nb), cap - len(nb)))


def do_patch(ko, table, vermagic=None, exact=False, dry=False):
    v, x, secs, data = read_tables(ko)
    if not v and not x:
        print(f"{ko}: 无版本表")
        return
    same = patched = 0
    missing = []
    with open(ko, "r+b") as f:
        for sym, (off, crc) in v.items():
            if sym not in table:
                missing.append(sym)
                continue
            want = table[sym]
            if want == crc:
                same += 1
                continue
            if not dry:
                f.seek(off)
                f.write(struct.pack("<I", want))
                if sym in x:
                    f.seek(x[sym][0])
                    f.write(struct.pack("<I", want))
            patched += 1
            tag = "+ext" if sym in x else "(versions only!)"
            print(f"  {sym}: 0x{crc:08x} -> 0x{want:08x} {tag}")
        if vermagic is not None:
            patch_vermagic(f, data, secs, vermagic, exact, dry)
    print(f"{ko}: 改写 {patched}, 已一致 {same}, 缺表 {len(missing)}")
    if missing:
        print(f"  未覆盖: {', '.join(sorted(missing))}")


def dump_modinfo(ko):
    """打印 .modinfo 全部条目, 用于确认设备侧 vermagic 真值。"""
    secs, data = elf_info(ko)
    mi = secs.get(".modinfo")
    if not mi:
        print(f"{ko}: 无 .modinfo")
        return
    off, size = mi
    blob = data[off:off + size]
    print(f"{ko}: .modinfo @0x{off:x} size={size}")
    for e in [x for x in blob.split(b"\0") if x]:
        print("   %s" % e.decode("ascii", "replace"))


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--help" in argv or "-h" in argv:
        raise SystemExit(__doc__)
    if "--dump-modinfo" in argv:
        for ko in [a for a in argv if not a.startswith("--")]:
            dump_modinfo(ko)
        raise SystemExit(0)

    args = [a for a in argv if not a.startswith("--")]
    vm = next((a.split("=", 1)[1] for a in argv if a.startswith("--vermagic=")), None)
    exact = "--vermagic-exact" in argv
    dry = "--dry-run" in argv
    if len(args) < 2:
        raise SystemExit(__doc__)
    with open(args[0]) as f:
        tbl = json.load(f)
    print(f"CRC 表 {args[0]}: {len(tbl)} 个符号" + ("  [dry-run]" if dry else ""))
    for ko in args[1:]:
        do_patch(ko, tbl, vm, exact, dry)
