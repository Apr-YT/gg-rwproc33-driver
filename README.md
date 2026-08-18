# gg-rwproc33-driver

GameGuardian 内核内存读写/硬件断点驱动的**私有备份与 6.12.69 适配**仓库。

## 目录

| 目录 | 内容 |
|---|---|
| `rwproc33/` | rwproc33 驱动源码（含上游历史 + 本地适配 commit） |
| `kernel-assets/` | 真机内核提取/适配资产（非源码，备份用） |

## 来源说明

- `rwproc33/` 上游：<https://github.com/abcz316/rwProcMem33>（第三方开源，尊重其 LICENSE/声明）
- 本仓库为**私有备份 + 真机适配**：`6.12.69` 内核（KernelSU）insmod 验证通过
  - rwProcMem_module（内存读写）
  - hwBreakpointProc_module（perf_event 硬件断点）

## kernel-assets 说明

- `kern/`：真机内核提取（kernel.bin / kernel.elf）
- `device_loaded.ko` / `device_sample.ko`：真机已加载的驱动备份
- `device_kallsyms.txt` / `device_config.txt`：真机 kallsyms / 配置导出
- `cfi_patch/`：5.x 内核去除 CFI 验证的工具包（与 rwproc33 内自带 .7z 对应）

> 注意：本仓库为自用备份，若需公开分发驱动源码请遵循上游 LICENSE 与原作者授权。
