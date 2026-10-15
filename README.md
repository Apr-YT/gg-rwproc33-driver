# gg-rwproc33-driver

GameGuardian 内核内存读写 / 硬件断点驱动的**私有备份与真机适配**仓库。

## 目录

| 目录 | 内容 |
|---|---|
| `rwproc33/` | rwproc33 驱动源码（上游历史 + 本地适配 commit） |
| `kernel-assets/` | 真机内核提取 / 适配资产（非源码，备份用） |
| `tools/` | MODVERSIONS CRC 流水线：提取内核 CRC 表 → 修补 `.ko` → 交叉校验 |

## 来源说明

- `rwproc33/` 上游：<https://github.com/abcz316/rwProcMem33>（第三方开源，遵循上游 LICENSE / 声明）
- 本仓库为**私有备份 + 真机适配**，不对外分发驱动源码

## 真机验证状态

设备内核 `6.12.69-android16-6-g586bfab1b9c5-abogki536749445-4k`（Android 17），KernelSU 环境：

| 模块 | `insmod` 结果 | init 日志 |
|---|---|---|
| `rwProcMem_module.ko` | **rc=0** | `[rwcfi] cfi_bypass patched: 1` + `Hello` |
| `hwBreakpointProc_module.ko` | **rc=0** | `[rwcfi] cfi_bypass patched: 0` + `Hello` |

dmesg 中 **0 条** `disagrees about version of symbol` / `version magic`。

> `/proc/modules` 里查不到属**预期** —— 两个模块 init 即自隐藏。

## kernel-assets 说明

> ⚠️ **本目录混有两套不同内核的资产，不可混用。** `kern/` 里的镜像**不是设备内核**。

| 文件 | 归属内核 | 说明 |
|---|---|---|
| `kern/kernel.bin`、`kern/kernel.elf` | **6.12.23-android16-5** | 早期提取的 GKI Image（保留备用，非设备内核） |
| `kcrctab_6.12.23.json` | **6.12.23-android16-5** | 由上面镜像提取的 CRC 表（8732 符号）。**未用真实 6.12.23 模块交叉验证**，仅供该内核参考 |
| `device_loaded.ko`、`device_sample.ko` | 6.12.69-android16-6 | 真机已加载驱动备份 |
| `device_kallsyms.txt`、`device_config.txt` | 6.12.69-android16-6 | 真机 kallsyms / 配置导出（`kptr_restrict=2`，地址全 0，仅可用于排序） |
| **`kcrctab_6.12.69.json`** | 6.12.69-android16-6 | ✅ **权威 CRC 全表**（9884 符号），与真机 KO 交叉校验**冲突 = 0** |
| `cfi_patch/` | — | 5.x 内核去除 CFI 验证的工具包 |

### ⚠️ 关键坑：拿错内核镜像 = 0 命中

两套资产实测**符号集交集仅 4823 / 9884**，交集中 CRC 相同率仅 **28.8%**。
用 6.12.23 的表去修补设备模块，上机必然 `disagrees about version of symbol`。

**判别方法**（不必启动设备）：

```bash
python -c "import re;d=open('kernel.elf','rb').read();print(re.search(rb'Linux version ([^\x00]{0,80})',d).group(1))"
# 6.12.23-android16-5-ga8f88ad96df3-ab13929693-4k     ← 与 uname -r 不符即为错镜像
```

**真机内核需自行从设备提取**（本机已验证路径）：

```bash
adb shell 'su -c "ls -l /dev/block/by-name/boot_a"'      # → /dev/block/sde14
adb shell 'su -c "dd if=/dev/block/sde14 of=/data/local/tmp/boot_a.img"'
adb pull /data/local/tmp/boot_a.img
# 按 Android boot header 解析：magic "ANDROID!"，header_version=4，
# kernel 段位于偏移 4096，长度 = header.kernel_size（本机为 0x27a6a00）
# 取出后文件偏移 0x38 处应为 ARM\x64（说明是扁平 ARM64 Image，非 ELF）
```

## MODVERSIONS CRC 流水线（`tools/`）

内核开 `CONFIG_MODVERSIONS` + `CONFIG_EXTENDED_MODVERSIONS=y` 时，`insmod` 逐符号比对
CRC，**且只读 `__version_ext_crcs`**（`__versions` 仅兼容保留 —— 只改它会静默无效）。
自编译 `.ko` 的 CRC 来自 vanilla GKI，须按真机内核重写。

```bash
# 1) 从真机内核 Image 提取全量导出符号 CRC 表（自动定位 __ksymtab / __kcrctab）
python tools/extract_kernel_crc.py kernel_raw.bin \
       --verify kernel-assets/device_sample.ko \
       -o kernel-assets/kcrctab_6.12.69.json

# 2) 双写修补 .ko 的两张版本表（vermagic 默认自动继承原标志串）
python tools/patch_ko_crc.py kernel-assets/kcrctab_6.12.69.json \
       rwProcMem_module.ko hwBreakpointProc_module.ko

# 3) 交叉校验（判据：冲突 = 0；“未覆盖”只表示该符号非内核导出，不算错）
python tools/crosscheck_ko_crc.py kernel-assets/kcrctab_6.12.69.json *.ko
```

> ⚠️ **vermagic 是比 CRC 更靠前的闸门。** `load_module()` 顺序为
> `check_modinfo()`（vermagic）→ … → `simplify_symbols()`（CRC）；
> vermagic 不过**根本走不到** CRC 校验。且模块带 `__versions` 时
> `same_magic()` 只比对首个空格之后的标志串：
>
> ```c
> if (has_crcs) { amagic += strcspn(amagic, " "); bmagic += strcspn(bmagic, " "); }
> return strcmp(amagic, bmagic) == 0;
> ```
>
> 所以改写 vermagic 只写版本号会把标志串填成 NUL → 必然 `-ENOEXEC`。
> `patch_ko_crc.py` 默认自动继承标志串，`--vermagic-exact` 可关，`--dump-modinfo` 查看设备真值。

脚本细节与完整踩坑清单见 [`tools/README.md`](tools/README.md)。

## 免责

本仓库为自用备份。公开分发驱动源码请遵循上游 LICENSE 与原作者授权；
`kernel-assets/` 下的内核镜像 / kallsyms / 配置均来自自有设备，请勿外传。
