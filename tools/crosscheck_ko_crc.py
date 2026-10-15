#!/usr/bin/env python3
"""crosscheck_ko_crc.py — 用内核 CRC 全表交叉校验一批 .ko 的覆盖情况

用途
----
验证 extract_kernel_crc.py 产出的 __kcrctab 全表是否可信:
对每个 .ko 读其自带的 __version_ext_crcs / __versions, 与表逐符号比对。
**判据: 冲突 = 0 即通过**; "未覆盖"只表示该符号不在内核导出表里
(私有/非导出符号, 例如 qcom_*、cfg80211_*, 或 6.10+ 已变宏的 kmalloc),
不影响正确性。

用法
----
  python crosscheck_ko_crc.py <kcrctab.json> <a.ko> [b.ko ...]
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_kernel_crc import read_ko_crcs  # noqa: E402


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    table = json.load(open(sys.argv[1], encoding="utf-8"))
    print(f"CRC 表 {os.path.basename(sys.argv[1])}: {len(table)} 个符号")
    bad = 0
    for ko in sys.argv[2:]:
        c = read_ko_crcs(ko)
        if not c:
            print(f"  {os.path.basename(ko):28s} （无版本表, 跳过）")
            continue
        hit = sum(1 for s, v in c.items() if table.get(s) == v)
        wrong = [s for s in c if s in table and table[s] != c[s]]
        miss = [s for s in c if s not in table]
        nm = os.path.basename(ko)
        print(f"  {nm:28s} 一致 {hit:4d} / 冲突 {len(wrong):3d} / 未覆盖 {len(miss):3d} (共 {len(c)})")
        for s in wrong[:8]:
            print(f"      !! 冲突 {s}: ko=0x{c[s]:08x} table=0x{table[s]:08x}")
        if wrong:
            bad += 1
        else:
            print("      => 校验通过 (0 冲突)")
        if miss:
            shown = ", ".join(sorted(miss)[:10])
            print(f"      未覆盖样例: {shown}{' ...' if len(miss) > 10 else ''}")
    if bad:
        raise SystemExit(f"[FAIL] {bad} 个模块存在冲突 —— CRC 表或 KO 有问题")
    print("[OK] 全部模块 0 冲突")


if __name__ == "__main__":
    main()
