#ifndef _HWBP_PROC_H_
#define _HWBP_PROC_H_
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/compat.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/ksm.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/pid.h>
#include <linux/slab.h> //kmalloc与kfree
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include "ver_control.h"
#include "arm64_register_helper.h"
#ifdef CONFIG_USE_PROC_FILE_NODE
#include <linux/proc_fs.h>
#endif
//////////////////////////////////////////////////////////////////

/* ⚠️ 命令码必须显式从 1 开始（与 agentd driver.h RWBP_CMD_* / 上游协议一致）！
 * 2026-08-17 根因: 原枚举从 0 开始 → 与用户态(agentd/bptest 按 1 起始)整体错位 +1,
 *   gg_watch_set 发 cmd=1(以为 OPEN) 实际触发 CLOSE → put_pid(pid) 解引用坏指针 → 内核挂死。 */
enum {
	CMD_OPEN_PROCESS = 1, 			// 打开进程
	CMD_CLOSE_PROCESS = 2, 			// 关闭进程
	CMD_GET_NUM_BRPS = 3, 			// 获取CPU硬件执行断点支持数量
	CMD_GET_NUM_WRPS = 4, 			// 获取CPU硬件访问断点支持数量
	CMD_INST_PROCESS_HWBP = 5,		// 安装进程硬件断点
	CMD_UNINST_PROCESS_HWBP = 6,	// 卸载进程硬件断点
	CMD_SUSPEND_PROCESS_HWBP = 7,	// 暂停进程硬件断点
	CMD_RESUME_PROCESS_HWBP = 8,	// 恢复进程硬件断点
	CMD_GET_HWBP_HIT_COUNT = 9,		// 获取硬件断点命中地址数量
	CMD_GET_HWBP_HIT_DETAIL = 10,	// 获取硬件断点命中详细信息
	CMD_SET_HOOK_PC = 11,			// 设置无条件Hook跳转
	CMD_HIDE_KERNEL_MODULE = 12,	// 隐藏驱动
	CMD_INST_PROCESS_HWBP_ALL = 13,	// 对进程全部线程批量安装硬件断点（驱动内 for_each_thread 枚举）
};

struct hwBreakpointProcDev {
#ifdef CONFIG_USE_PROC_FILE_NODE
	struct proc_dir_entry *proc_parent;
	struct proc_dir_entry *proc_entry;
#endif
	bool is_hidden_module; //是否已经隐藏过驱动列表了
};
static struct hwBreakpointProcDev *g_hwBreakpointProc_devp;

static ssize_t hwBreakpointProc_read(struct file* filp, char __user* buf, size_t size, loff_t* ppos);
static int hwBreakpointProc_release(struct inode *inode, struct file *filp);
static const struct proc_ops hwBreakpointProc_proc_ops = {
    .proc_read    = hwBreakpointProc_read,
	.proc_release = hwBreakpointProc_release,
};

#pragma pack(1)
struct my_user_pt_regs {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint64_t orig_x0;
	uint64_t syscallno;
};
struct HWBP_HIT_ITEM {
	uint64_t task_id;
	uint64_t hit_addr;
	uint64_t hit_time;
	struct my_user_pt_regs regs_info;
};
#pragma pack()

/* 命中记录上限（与上游 MIN_LEN 一致; agentd 按此做索引去重, 不可随意改小） */
#define HWBP_HIT_CAP 512
/* 最大并发断点句柄数（固定槽位数组, 避免 IRQ 上下文动态分配）。
 * 2026-08-18 通用化: 16 → 128 —— CMD_INST_PROCESS_HWBP_ALL 全线程注册,
 * 游戏进程线程数可达 70+, 槽位必须覆盖全部线程（per-task 断点每线程独立 debug 槽）。 */
#define HWBP_MAX_HANDLES 128

/*
 * 断点句柄。存储设计（6.12 修复）:
 *  - 命中回调运行在 ARM64 调试异常上下文（中断/抢占关闭）→ 任何 sleep（mutex/
 *    vmalloc/modify_user_hw_breakpoint）都会触发 "scheduling while atomic" BUG,
 *    本设备 PANIC_ON_OOPS=y 直接重启。
 *  - 因此: 句柄用固定数组 g_hwbp_handles[] + raw_spinlock 保护; 命中缓冲 hit_arr
 *    在 INST 时（进程上下文）一次性 vmalloc 预分配 HWBP_HIT_CAP 条, 回调里只做
 *    memcpy 写入, 绝不分配/加锁睡眠。
 */
struct HWBP_HANDLE_INFO {
	bool in_use;                /* 槽位占用标记 */
	bool suspended;             /* SUSPEND/RESUME: 暂停/恢复记录（回调只读, 锁保护） */
	uint64_t task_id;           /* 目标进程 pid */
	struct perf_event * sample_hbp;
	struct perf_event_attr original_attr;
	bool is_32bit_task;
	size_t hit_total_count;     /* 命中总次数（不截断） */
	size_t hit_count;           /* 已记录条目数（<= HWBP_HIT_CAP, 追加式, 与 agentd 索引去重契约一致） */
	struct HWBP_HIT_ITEM * hit_arr;   /* vmalloc 预分配, HWBP_HIT_CAP 条 */
};

#endif /* _HWBP_PROC_H_ */