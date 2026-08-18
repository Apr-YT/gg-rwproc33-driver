#ifndef VER_CONTROL_H_
#define VER_CONTROL_H_
#include <linux/version.h>

// 独立内核模块入口模式
#define CONFIG_MODULE_GUIDE_ENTRY

// 生成proc用户层交互节点文件
#define CONFIG_USE_PROC_FILE_NODE
// 隐蔽通信密钥
#define CONFIG_PROC_NODE_AUTH_KEY "dce3771681d4c7a143d5d06b7d32548e"

// 调试打印模式（诊断期开; 生产关闭减少刷屏）
//#define CONFIG_DEBUG_PRINTK

// 动态寻址模式
#define CONFIG_KALLSYMS_LOOKUP_NAME

// 精准命中记录模式
// ⚠️ 6.12 设备上已禁用: 命中回调在 ARM64 调试异常上下文（中断/抢占关闭）里调用
//    modify_user_hw_breakpoint()（内部 perf_event_disable → ctx->mutex）与
//    toggle_bp_registers_directly()，会触发 "scheduling while atomic" BUG；
//    本设备 CONFIG_PANIC_ON_OOPS=y → 直接重启。断点命中记录改用预分配环形缓冲
//    + raw_spinlock（见 hwBreakpointProc_module.c hwbp_handler）。
//#define CONFIG_MODIFY_HIT_NEXT_MODE

// 反PTRACE侦测模式
// ⚠️ 6.12 设备上已禁用: kretprobe 回调（arch_ptrace）在 kprobe 上下文里调用
//    mutex_lock + __arch_copy_from_user，同样是原子上下文违规 → PANIC_ON_OOPS 重启。
//#define CONFIG_ANTI_PTRACE_DETECTION_MODE

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif
#ifndef MY_LINUX_VERSION_CODE 
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,10,0)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,10,84)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,18,71)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(3,18,140)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,21)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,78)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,153)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,4,192)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,9,112)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,9,186)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,14,83)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,14,117)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,14,141)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,19,81)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(4,19,113)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,4,61)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,10,43)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(5,15,41)
//#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,6,30)
#define MY_LINUX_VERSION_CODE KERNEL_VERSION(6,6,30)   /* 作者最新分支（proc_maps.h 有 6.6.30 实现; 真机验证过） */
#endif

#ifdef CONFIG_DEBUG_PRINTK
#define printk_debug printk
#else
static inline void printk_debug(char *fmt, ...) {}
#endif

#endif /* VER_CONTROL_H_ */
