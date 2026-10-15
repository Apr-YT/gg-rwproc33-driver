# tools — MODVERSIONS CRC 流水线

三个独立脚本，**仅依赖 Python 3 标准库**（无第三方包，离线可用）。

| 脚本 | 作用 |
|---|---|
| `extract_kernel_crc.py` | 从 ARM64 内核 Image 自动定位 `__ksymtab` / `__ksymtab_gpl`，投票反解 `__kcrctab` / `__kcrctab_gpl` 基址，输出「符号 → CRC」JSON 全表 |
| `patch_ko_crc.py` | 用 CRC 表**双写** `.ko` 的 `__versions` + `__version_ext_crcs`；可选改写 `vermagic`（默认自动继承原标志串） |
| `crosscheck_ko_crc.py` | 用 CRC 表交叉校验一批 `.ko`；**判据：冲突 = 0** |

> `crosscheck_ko_crc.py` 会 `import extract_kernel_crc`，三个脚本需保持同目录。

## 为什么需要它

内核开启 `CONFIG_MODVERSIONS` + `CONFIG_EXTENDED_MODVERSIONS=y` 时，`insmod` 会逐符号
比对 CRC，**且只读 `__version_ext_crcs`**（`__versions` 仅为兼容保留）。自编译 `.ko` 的
CRC 来自 vanilla GKI，与厂商改动内核不符 → `disagrees about version of symbol xxx`。

**只改 `__versions` 是静默无效的** —— 内核根本不看它。必须双写。

## 典型用法

```bash
# 1) 提取：从真机内核 Image 生成权威 CRC 表
python extract_kernel_crc.py kernel_raw.bin \
       --verify device_sample.ko \
       -o kcrctab_6.12.69.json

# 2) 修补：双写两表 + 对齐 vermagic
python patch_ko_crc.py kcrctab_6.12.69.json \
       rwProcMem_module.ko hwBreakpointProc_module.ko

# 3) 校验：冲突必须为 0
python crosscheck_ko_crc.py kcrctab_6.12.69.json *.ko
```

### 提取（`extract_kernel_crc.py`）

```bash
python extract_kernel_crc.py <内核Image> [-o 输出.json] [--verify <参考.ko> ...]
```

* 输入须是**扁平 ARM64 Image**（magic `ARM\x64` 位于文件偏移 `0x38`），**不是 ELF**。
  手上若是 `boot.img`，先按 Android boot header 取出 kernel 段（见仓库根 README）。
* `--verify` 可传一个真机模块：脚本会用它的 `__version_ext_crcs` 反向投票验证基址，
  **0 冲突**才算可信。
* 耗时约 1 分钟（暴力扫描，纯 Python）。

### 修补（`patch_ko_crc.py`）

```bash
python patch_ko_crc.py <crc.json> <目标.ko> [...] [选项]

  --vermagic=<字符串>   改写 vermagic；**默认自动继承原标志串**（只写版本号会把
                        " SMP preempt mod_unload modversions aarch64" 截成 NUL → 必然失败）
  --vermagic-exact      严格按给定值写，不继承标志串（仅在你清楚后果时用）
  --dump-modinfo        只打印 .modinfo 里的 name/vermagic/depends，不修改
  --dry-run             只报告不写入
```

### 校验（`crosscheck_ko_crc.py`）

```bash
python crosscheck_ko_crc.py <crc.json> <目标.ko> [...]
```

输出 `一致 / 冲突 / 未覆盖` 三项。**只有「冲突」代表错误**。

## 已知边界（「未覆盖」≠ 缺陷）

* 6.10+ 起 `kmalloc` / `printk` / `vmalloc` / `raw_spin_lock_irqsave` 已改为宏或内联，
  导出名是 `__kmalloc_noprof` / `_printk` 之类；表里查不到旧名是正常的。
* `kallsyms_lookup_name`（5.7 起）本就**不再导出**；设备未导出的符号模块会用
  kprobe 自行解析，与 CRC 表无关。
* 厂商私有导出（`qcom_*`、`cfg80211_*` 等）不在上游导出表里，报「未覆盖」属预期。

## 关键坑

1. **拿错内核镜像 = 0 命中。** 本仓库 `kernel-assets/kern/` 是 **6.12.23-android16-5**，
   不是设备内核。判别与提取见仓库根 README。
2. **vermagic 是比 CRC 更靠前的闸门。** `load_module()` 顺序为 `check_modinfo()`
   → … → `simplify_symbols()`；vermagic 不过**根本走不到** CRC 校验。且模块带
   `__versions` 时 `same_magic()` 只比对首个空格之后的标志串。
3. **`__version_ext_names` 是 NUL 分隔**的，不是长度前缀。
4. **`__ksymtab` 的 name 目标不保证单调**（条目按符号名排序，字符串按 .o 顺序排布），
   扫描时不能加单调约束。
5. **msys / Git Bash 会静默吃掉 CR**：本机「跑通了」不能证明 CRLF 相关问题正确，
   要按字节查（`git ls-files --eol`、`tr -cd '\r' < f | wc -c`）。
