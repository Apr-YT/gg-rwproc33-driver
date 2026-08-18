/*
 * hwBreakpointProc_module.c — 硬件断点/监视点 proc 节点模块
 *
 * 6.12 真机崩溃修复（2026-08-17）:
 *
 *  ⭐ 根因（cmd 枚举错位, 已修复）:
 *    模块原命令枚举从 0 开始（CMD_OPEN_PROCESS=0, CMD_CLOSE_PROCESS=1, ...）,
 *    而用户态（agentd driver.h RWBP_CMD_* / bptest / 上游协议）按 1 起始发送。
 *    所有命令错位 +1: gg_watch_set 发 cmd=1（以为是 OPEN）实际触发 CMD_CLOSE_PROCESS
 *    → put_pid((struct pid*)pid) 解引用用户态整数 → 内核数据异常 → 整机静默挂死
 *    （设备 panic=-1 + 看门狗重启, 无任何内核日志）。
 *    现已显式从 1 开始枚举（见 hwBreakpointProc_module.h）。
 *
 *  修复（防御性, 一并做了）:
 *    - 命中回调运行在 ARM64 调试异常上下文（中断/抢占关闭）, 原代码却
 *      mutex_lock / cvector_pushback(vmalloc) / modify_user_hw_breakpoint
 *      （perf_event_disable → ctx->mutex）→ 原子上下文睡眠 BUG → 已改为
 *      固定槽位数组 + raw_spinlock + INST 时预分配命中缓冲, 回调只 memcpy。
 *    - 命中回调对所有类型（W/R/RW/X）直接清硬件槽 E 位防重入:
 *      ARM64 监视点/断点命中返回后指令会重新执行, 不清 E 位 → 无限调试异常
 *      循环 → 整机挂死/看门狗重启。内核在任务切回时会按 perf 事件重新武装
 *      该槽, 因此命中是"每次切回后可持续捕获"（与上游 #else 分支语义一致）。
 *    - 禁用 CONFIG_MODIFY_HIT_NEXT_MODE / CONFIG_ANTI_PTRACE_DETECTION_MODE
 *      （见 ver_control.h 注释）与 hide_procfs_dir 的 kprobe。
 *
 *  协议（对 agentd 不变）:
 *    - 句柄/断点 ID 写回 buf+33（数据区起点）, read() 返回值 0 表示成功。
 *    - COUNT 写 {hit_total_count, hit_item_arr_count} 16 字节; DETAIL 按条目拷贝。
 *    - 命中条目 HWBP_HIT_ITEM（packed, sizeof 312）。
 */
#include "hwBreakpointProc_module.h"
#include "proc_pid.h"
#include "api_proxy.h"

#pragma pack(push,1)
struct ioctl_request {
    char     cmd;        /* 1 字节命令 */
    uint64_t param1;     /* 参数1 */
    uint64_t param2;     /* 参数2 */
    uint64_t param3;     /* 参数3 */
    uint64_t buf_size;    /* 紧随其后的动态数据长度 */
};
#pragma pack(pop)
//////////////////////////////////////////////////////////////////

static atomic64_t g_hook_pc;

/* 固定槽位 + IRQ 安全锁（命中回调上下文可用） */
static raw_spinlock_t g_hwbp_lock;
static struct HWBP_HANDLE_INFO g_hwbp_handles[HWBP_MAX_HANDLES];

/* 命中记录（调用方必须已持有 g_hwbp_lock, 且处于原子上下文——只做 memcpy） */
static void record_hit_locked(struct HWBP_HANDLE_INFO *info, struct pt_regs *regs) {
    struct HWBP_HIT_ITEM *it;
    if (!info || !regs || !info->hit_arr) return;
    if (info->hit_count >= HWBP_HIT_CAP) return;
    it = &info->hit_arr[info->hit_count];
    it->task_id = info->task_id;
    it->hit_addr = regs->pc;
    it->hit_time = ktime_get_real_seconds();
    memcpy(&it->regs_info.regs, regs->regs, sizeof(it->regs_info.regs));
    it->regs_info.sp = regs->sp;
    it->regs_info.pc = regs->pc;
    it->regs_info.pstate = regs->pstate;
    it->regs_info.orig_x0 = regs->orig_x0;
    it->regs_info.syscallno = regs->syscallno;
    info->hit_count++;
}

/*
 * 断点命中回调。⚠️ 运行在 ARM64 调试异常上下文（中断/抢占关闭, NMI 语义）:
 * 严禁 mutex/vmalloc/modify_user_hw_breakpoint/printk 长路径。
 */
static void hwbp_handler(struct perf_event *bp,
	struct perf_sample_data *data,
	struct pt_regs *regs) {
	uint64_t hook_pc;
	unsigned long flags;
	int i;

	hook_pc = atomic64_read(&g_hook_pc);
	if (hook_pc) {
		regs->pc = hook_pc;
		return;
	}

	printk_debug(KERN_INFO "hwbp HIT bp:%px pc:%px\n", bp, (void*)regs->pc);

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		struct HWBP_HANDLE_INFO *h = &g_hwbp_handles[i];
		if (!h->in_use || h->sample_hbp != bp) {
			continue;
		}
		if (h->suspended) {
			break;   /* SUSPEND 状态: 不记录 */
		}
		h->hit_total_count++;
		record_hit_locked(h, regs);
		/* ⚠️ 必须清硬件槽 E 位（所有类型）:
		 * ARM64 监视点/断点命中后, 返回时指令会重新执行 → 若不禁止该槽,
		 * 立即再次命中 → 无限调试异常循环 → 系统挂死 → 看门狗重启
		 * （2026-08-17 真机复现: 第一次实测就整机重启, pstore 无记录）。
		 * 内核在任务上下文切换时会按 perf 事件重新武装该槽, 因此实际上
		 * 每次切回后仍可持续捕获, 与上游 #else 分支语义一致。 */
		toggle_bp_registers_directly(&h->original_attr, h->is_32bit_task, 0);
		break;
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
}

static ssize_t OnCmdOpenProcess(struct ioctl_request *hdr, char __user* buf) {
	uint64_t pid = hdr->param1, handle = 0;
	struct pid * proc_pid_struct = NULL;
	printk_debug(KERN_INFO "CMD_OPEN_PROCESS pid:%llu\n", pid);

	proc_pid_struct = get_proc_pid_struct(pid);
	if (!proc_pid_struct) {
		return -EINVAL;
	}
	handle = (uint64_t)proc_pid_struct;

	if (!!x_copy_to_user((void*)buf, (void*)&handle, sizeof(handle))) {
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdCloseProcess(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	printk_debug(KERN_INFO "CMD_CLOSE_PROCESS\n");
	if (!proc_pid_struct) {
		return -EINVAL;
	}
	release_proc_pid_struct(proc_pid_struct);
	return 0;
}

static ssize_t OnCmdGetCpuNumBrps(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_GET_NUM_BRPS\n");
	return getCpuNumBrps();
}

static ssize_t OnCmdGetCpuNumWrps(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_GET_NUM_WRPS\n");
	return getCpuNumWrps();
}

static ssize_t OnCmdInstProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	uint64_t proc_virt_addr = hdr->param2;
	char hwbp_len  =  hdr->param3 & 0xFF;
	char hwbp_type = (hdr->param3 >> 8) & 0xFF;
	pid_t pid_val;
	struct task_struct *task;
	struct HWBP_HANDLE_INFO *slot = NULL;
	struct perf_event *bp;
	unsigned long flags;
	int i;
	printk_debug(KERN_INFO "CMD_INST_PROCESS_HWBP pid_struct:%px addr:%px len:%d type:%d\n",
		proc_pid_struct, (void*)proc_virt_addr, hwbp_len, hwbp_type);

	pid_val = pid_nr(proc_pid_struct);
	if (!pid_val) {
		printk_debug(KERN_INFO "pid_nr failed.\n");
		return -EINVAL;
	}

	task = pid_task(proc_pid_struct, PIDTYPE_PID);
	if (!task) {
		printk_debug(KERN_INFO "get_pid_task failed.\n");
		return -EINVAL;
	}

	/* 找空槽并立即占用（锁内预留, 防并发 INST 双占同一槽;
	 * 此窗口 sample_hbp==NULL, 命中回调按 bp 匹配必然跳过, 安全） */
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (!g_hwbp_handles[i].in_use) {
			slot = &g_hwbp_handles[i];
			memset(slot, 0, sizeof(*slot));
			slot->in_use = true;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	if (!slot) {
		printk_debug(KERN_INFO "no free handle slot\n");
		return -ENOSPC;
	}

	slot->task_id = pid_val;
	slot->is_32bit_task = is_compat_thread(task_thread_info(task));
	ptrace_breakpoint_init(&slot->original_attr);
	slot->original_attr.bp_addr = proc_virt_addr;
	slot->original_attr.bp_len = hwbp_len;
	slot->original_attr.bp_type = hwbp_type;
	slot->original_attr.disabled = 0;

	/* 预分配命中缓冲（进程上下文, vmalloc 安全） */
	slot->hit_arr = vmalloc(sizeof(struct HWBP_HIT_ITEM) * HWBP_HIT_CAP);
	if (!slot->hit_arr) {
		printk_debug(KERN_INFO "hit_arr vmalloc failed\n");
		raw_spin_lock_irqsave(&g_hwbp_lock, flags);
		slot->in_use = false;
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
		return -ENOMEM;
	}
	memset(slot->hit_arr, 0, sizeof(struct HWBP_HIT_ITEM) * HWBP_HIT_CAP);

	bp = x_register_user_hw_breakpoint(&slot->original_attr, hwbp_handler, NULL, task);
	if (IS_ERR((void __force *)bp)) {
		int ret = PTR_ERR((void __force *)bp);
		printk_debug(KERN_INFO "register_user_hw_breakpoint failed: %d\n", ret);
		raw_spin_lock_irqsave(&g_hwbp_lock, flags);
		slot->in_use = false;
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
		vfree(slot->hit_arr);
		slot->hit_arr = NULL;
		slot->sample_hbp = NULL;
		return ret;
	}
	slot->sample_hbp = bp;

	if (x_copy_to_user((void*)buf, &bp, sizeof(uint64_t))) {
		/* 用户缓冲不足: 撤销 */
		raw_spin_lock_irqsave(&g_hwbp_lock, flags);
		slot->in_use = false;
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
		vfree(slot->hit_arr);
		slot->hit_arr = NULL;
		slot->sample_hbp = NULL;
		x_unregister_hw_breakpoint(bp);
		return -EINVAL;
	}
	return 0;
}

/* 对进程全部线程批量安装硬件断点（通用入口, CMD_INST_PROCESS_HWBP_ALL=13）。
 * 驱动内 for_each_thread 枚举全部线程, 每线程注册一个 per-task 断点
 * （任务切换自动换装 debug 寄存器, 不受每 CPU WRP 4 槽限制）。
 * 入参: param1=handle(pid_struct*), param2=addr, param3=(len | type<<8)
 * 返回: buf[0..4)=注册数 N, buf[4..4+8N)=perf_event* 句柄数组。成功 0, 失败负 errno。 */
static ssize_t OnCmdInstAllProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	uint64_t proc_virt_addr = hdr->param2;
	char hwbp_len  = hdr->param3 & 0xFF;
	char hwbp_type = (hdr->param3 >> 8) & 0xFF;
	pid_t pid_val;
	struct task_struct *leader, *task;
	struct task_struct **tasks;	/* RCU 内收集的 task 引用（RCU 外逐个注册） */
	uint64_t *bp_ids;
	unsigned long flags;
	int n = 0, ntasks = 0, max_all = HWBP_MAX_HANDLES;
	int i, j;

	if (!proc_pid_struct) {
		return -EINVAL;
	}
	pid_val = pid_nr(proc_pid_struct);
	if (!pid_val) {
		return -EINVAL;
	}
	leader = pid_task(proc_pid_struct, PIDTYPE_TGID);
	if (!leader) {
		printk_debug(KERN_INFO "CMD_INST_ALL pid %d leader not found\n", pid_val);
		return -EINVAL;
	}

	bp_ids = (uint64_t *)vmalloc(sizeof(uint64_t) * max_all);
	if (!bp_ids) {
		return -ENOMEM;
	}
	/* ⚠️ 两阶段注册: for_each_thread 需 RCU 读锁, 但 vmalloc/register_user_hw_breakpoint
	 * 会睡眠 → RCU 锁内调用 = "scheduling while atomic" → PANIC_ON_OOPS 整机重启
	 * （2026-08-18 真机复现）。故 RCU 内只收集 task 引用, RCU 外逐个注册。 */
	tasks = (struct task_struct **)vmalloc(sizeof(*tasks) * max_all);
	if (!tasks) {
		vfree(bp_ids);
		return -ENOMEM;
	}
	ntasks = 0;

	rcu_read_lock();
	for_each_thread(leader, task) {
		if (ntasks >= max_all) {
			break;
		}
		get_task_struct(task);
		tasks[ntasks++] = task;
	}
	rcu_read_unlock();

	for (i = 0; i < ntasks; i++) {
		struct HWBP_HANDLE_INFO *slot = NULL;
		struct perf_event *bp;
		task = tasks[i];

		/* 找空槽并立即占用（锁内预留, 与单线程 INST 同一契约） */
		raw_spin_lock_irqsave(&g_hwbp_lock, flags);
		for (j = 0; j < HWBP_MAX_HANDLES; j++) {
			if (!g_hwbp_handles[j].in_use) {
				slot = &g_hwbp_handles[j];
				memset(slot, 0, sizeof(*slot));
				slot->in_use = true;
				break;
			}
		}
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
		if (!slot) {
			break;	/* 全部槽位占用 */
		}

		slot->task_id = task_pid_nr(task);
		slot->is_32bit_task = is_compat_thread(task_thread_info(task));
		ptrace_breakpoint_init(&slot->original_attr);
		slot->original_attr.bp_addr = proc_virt_addr;
		slot->original_attr.bp_len = hwbp_len;
		slot->original_attr.bp_type = hwbp_type;
		slot->original_attr.disabled = 0;

		slot->hit_arr = vmalloc(sizeof(struct HWBP_HIT_ITEM) * HWBP_HIT_CAP);
		if (!slot->hit_arr) {
			raw_spin_lock_irqsave(&g_hwbp_lock, flags);
			slot->in_use = false;
			raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
			continue;
		}
		memset(slot->hit_arr, 0, sizeof(struct HWBP_HIT_ITEM) * HWBP_HIT_CAP);

		bp = x_register_user_hw_breakpoint(&slot->original_attr, hwbp_handler, NULL, task);
		if (IS_ERR((void __force *)bp)) {
			int ret = PTR_ERR((void __force *)bp);
			raw_spin_lock_irqsave(&g_hwbp_lock, flags);
			slot->in_use = false;
			raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
			vfree(slot->hit_arr);
			slot->hit_arr = NULL;
			slot->sample_hbp = NULL;
			printk_debug(KERN_INFO "CMD_INST_ALL tid %d register failed: %d\n",
				task_pid_nr(task), ret);
			continue;	/* 单个线程失败跳过, 尽量全注册 */
		}
		slot->sample_hbp = bp;
		bp_ids[n++] = (uint64_t)bp;
	}

	/* 释放 task 引用（注册成功者 x_register_user_hw_breakpoint 已自持引用） */
	for (i = 0; i < ntasks; i++) {
		put_task_struct(tasks[i]);
	}
	vfree(tasks);

	if (!!x_copy_to_user((void*)buf, (void*)&n, sizeof(int))) {
		vfree(bp_ids);
		return -EINVAL;
	}
	if (n > 0 && !!x_copy_to_user((void*)(buf + sizeof(int)), (void*)bp_ids,
					n * sizeof(uint64_t))) {
		vfree(bp_ids);
		return -EINVAL;
	}
	vfree(bp_ids);
	printk_debug(KERN_INFO "CMD_INST_ALL pid %d addr %px len %d type %d -> %d/%d threads\n",
		pid_val, (void*)proc_virt_addr, hwbp_len, hwbp_type, n, ntasks);
	return 0;
}

static ssize_t OnCmdUninstProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event * sample_hbp = (struct perf_event *)hdr->param1;
	struct HWBP_HANDLE_INFO *slot = NULL;
	struct HWBP_HIT_ITEM *hit_arr = NULL;
	unsigned long flags;
	int i;
	printk_debug(KERN_INFO "CMD_UNINST_PROCESS_HWBP bp:%px\n", sample_hbp);
	if(!sample_hbp) {
		return -EFAULT;
	}

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use && g_hwbp_handles[i].sample_hbp == sample_hbp) {
			slot = &g_hwbp_handles[i];
			slot->in_use = false;
			hit_arr = slot->hit_arr;
			slot->hit_arr = NULL;
			slot->sample_hbp = NULL;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	if (slot) {
		if (hit_arr) vfree(hit_arr);
		x_unregister_hw_breakpoint(sample_hbp);
		return 0;
	}
	return -EFAULT;
}

static ssize_t OnCmdSuspendProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event * sample_hbp = (struct perf_event *)hdr->param1;
	unsigned long flags;
	int i;
	printk_debug(KERN_INFO "CMD_SUSPEND_PROCESS_HWBP bp:%px\n", sample_hbp);
	if(!sample_hbp) {
		return -EFAULT;
	}
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use && g_hwbp_handles[i].sample_hbp == sample_hbp) {
			g_hwbp_handles[i].suspended = true;
			raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
			return 0;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	return -EFAULT;
}

static ssize_t OnCmdResumeProcessHwbp(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event * sample_hbp = (struct perf_event *)hdr->param1;
	unsigned long flags;
	int i;
	printk_debug(KERN_INFO "CMD_RESUME_PROCESS_HWBP bp:%px\n", sample_hbp);
	if(!sample_hbp) {
		return -EFAULT;
	}
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use && g_hwbp_handles[i].sample_hbp == sample_hbp) {
			g_hwbp_handles[i].suspended = false;
			raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
			return 0;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	return -EFAULT;
}

static ssize_t OnCmdGetHwbpHitCount(struct ioctl_request *hdr, char __user* buf) {
	#pragma pack(1)
	struct buf_layout {
		uint64_t hit_total_count;
		uint64_t hit_item_arr_count;
	};
	#pragma pack()
	struct perf_event *sample_hbp = (struct perf_event *)hdr->param1;
	struct buf_layout user_data = {0};
	unsigned long flags;
	int i, found = 0;
	printk_debug(KERN_INFO "CMD_GET_HWBP_HIT_COUNT bp:%px\n", sample_hbp);

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use && g_hwbp_handles[i].sample_hbp == sample_hbp) {
			user_data.hit_total_count = g_hwbp_handles[i].hit_total_count;
			user_data.hit_item_arr_count = g_hwbp_handles[i].hit_count;
			found = 1;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	if (!found) return -EFAULT;

	printk_debug(KERN_INFO "counts: total=%zu arr=%zu\n",
		(size_t)user_data.hit_total_count, (size_t)user_data.hit_item_arr_count);
	if (x_copy_to_user((void*)buf, &user_data, sizeof(user_data))) {
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdGetHwbpHitDetail(struct ioctl_request *hdr, char __user* buf) {
	struct perf_event *sample_hbp = (struct perf_event *)hdr->param1;
	size_t size = hdr->buf_size;
	unsigned long flags;
	size_t avail = 0, take;
	int i, found = 0;
	void *tmp = NULL;
	ssize_t ret = 0;
	printk_debug(KERN_INFO "CMD_GET_HWBP_HIT_DETAIL bp:%px size:%zu\n", sample_hbp, size);

	/* 上限保护 */
	take = size / sizeof(struct HWBP_HIT_ITEM);
	if (take > HWBP_HIT_CAP) take = HWBP_HIT_CAP;
	if (take == 0) return 0;

	/* 先读可用条数（锁内只读） */
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use && g_hwbp_handles[i].sample_hbp == sample_hbp) {
			avail = g_hwbp_handles[i].hit_count;
			found = 1;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
	if (!found) return -EFAULT;
	if (take > avail) take = avail;
	if (take == 0) return 0;

	/* 锁外分配临时缓冲（vmalloc 可睡眠, 不能持锁） */
	tmp = vmalloc(take * sizeof(struct HWBP_HIT_ITEM));
	if (!tmp) return -ENOMEM;

	/* 锁内 memcpy（防止并发 UNINST vfree hit_arr） */
	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	found = 0;
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use && g_hwbp_handles[i].sample_hbp == sample_hbp) {
			avail = g_hwbp_handles[i].hit_count;
			if (take > avail) take = avail;
			if (take) {
				memcpy(tmp, g_hwbp_handles[i].hit_arr,
				       take * sizeof(struct HWBP_HIT_ITEM));
			}
			found = 1;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	if (!found) { vfree(tmp); return -EFAULT; }
	if (take == 0) { vfree(tmp); return 0; }

	ret = (ssize_t)(take * sizeof(struct HWBP_HIT_ITEM));
	if (x_copy_to_user((void*)buf, tmp, (unsigned long)ret)) {
		ret = -EINVAL;
	}
	vfree(tmp);
	return ret;
}

static ssize_t OnCmdSetHookPc(struct ioctl_request *hdr, char __user* buf) {
	uint64_t pc = hdr->param1;
	printk_debug(KERN_INFO "CMD_SET_HOOK_PC pc:%px\n", (void*)pc);
	atomic64_set(&g_hook_pc, pc);
	return 0;
}

static ssize_t OnCmdHideKernelModule(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_HIDE_KERNEL_MODULE\n");
	if (g_hwBreakpointProc_devp->is_hidden_module == false) {
		g_hwBreakpointProc_devp->is_hidden_module = true;
		list_del_init(&__this_module.list);
		kobject_del(&THIS_MODULE->mkobj.kobj);
	}
	return 0;
}

static inline ssize_t DispatchCommand(struct ioctl_request *hdr, char __user* buf) {
	switch (hdr->cmd) {
	case CMD_OPEN_PROCESS:
		return OnCmdOpenProcess(hdr, buf);
	case CMD_CLOSE_PROCESS:
		return OnCmdCloseProcess(hdr, buf);
	case CMD_GET_NUM_BRPS:
		return OnCmdGetCpuNumBrps(hdr, buf);
	case CMD_GET_NUM_WRPS:
		return OnCmdGetCpuNumWrps(hdr, buf);
	case CMD_INST_PROCESS_HWBP:
		return OnCmdInstProcessHwbp(hdr, buf);
	case CMD_INST_PROCESS_HWBP_ALL:
		return OnCmdInstAllProcessHwbp(hdr, buf);
	case CMD_UNINST_PROCESS_HWBP:
		return OnCmdUninstProcessHwbp(hdr, buf);
	case CMD_SUSPEND_PROCESS_HWBP:
		return OnCmdSuspendProcessHwbp(hdr, buf);
	case CMD_RESUME_PROCESS_HWBP:
		return OnCmdResumeProcessHwbp(hdr, buf);
	case CMD_GET_HWBP_HIT_COUNT:
		return OnCmdGetHwbpHitCount(hdr, buf);
	case CMD_GET_HWBP_HIT_DETAIL:
		return OnCmdGetHwbpHitDetail(hdr, buf);
	case CMD_SET_HOOK_PC:
		return OnCmdSetHookPc(hdr, buf);
	case CMD_HIDE_KERNEL_MODULE:
		return OnCmdHideKernelModule(hdr, buf);
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

static ssize_t hwBreakpointProc_read(struct file* filp,
                              char __user* buf,
                              size_t size,
                              loff_t* ppos) {
    struct ioctl_request hdr = {0};
    size_t header_size = sizeof(hdr);

    if (size < header_size) {
        return -EINVAL;
    }

    if (x_copy_from_user(&hdr, buf, header_size)) {
        return -EFAULT;
    }

    if (size < header_size + hdr.buf_size) {
        return -EINVAL;
    }

    return DispatchCommand(&hdr, buf + header_size);
}

/* 卸载全部断点（release / 模块退出）。vfree 与 unregister 均放在锁外。
 * 数组动态分配（HWBP_MAX_HANDLES=128 后静态数组超内核栈 2KB 限制）。 */
static void clean_hwbp(void) {
	struct perf_event **bps;
	struct HWBP_HIT_ITEM **arrs;
	unsigned long flags;
	int i, n = 0;

	bps = vmalloc(sizeof(*bps) * HWBP_MAX_HANDLES);
	arrs = vmalloc(sizeof(*arrs) * HWBP_MAX_HANDLES);
	if (!bps || !arrs) {
		/* 极端 OOM: 锁内直接清 in_use, 断点/缓冲由模块退出回收 */
		vfree(bps);
		vfree(arrs);
		raw_spin_lock_irqsave(&g_hwbp_lock, flags);
		for (i = 0; i < HWBP_MAX_HANDLES; i++) {
			g_hwbp_handles[i].in_use = false;
			g_hwbp_handles[i].sample_hbp = NULL;
			g_hwbp_handles[i].hit_arr = NULL;
		}
		raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);
		return;
	}

	raw_spin_lock_irqsave(&g_hwbp_lock, flags);
	for (i = 0; i < HWBP_MAX_HANDLES; i++) {
		if (g_hwbp_handles[i].in_use) {
			bps[n] = g_hwbp_handles[i].sample_hbp;
			arrs[n] = g_hwbp_handles[i].hit_arr;
			g_hwbp_handles[i].sample_hbp = NULL;
			g_hwbp_handles[i].hit_arr = NULL;
			g_hwbp_handles[i].in_use = false;
			n++;
		}
	}
	raw_spin_unlock_irqrestore(&g_hwbp_lock, flags);

	for (i = 0; i < n; i++) {
		if (arrs[i]) vfree(arrs[i]);
		if (bps[i]) x_unregister_hw_breakpoint(bps[i]);
	}
	vfree(bps);
	vfree(arrs);
}

static int hwBreakpointProc_release(struct inode *inode, struct file *filp) {
	clean_hwbp();
	return 0;
}

static int hwBreakpointProc_dev_init(void) {
#ifdef CONFIG_KALLSYMS_LOOKUP_NAME
	if(!init_kallsyms_lookup()) {
		printk(KERN_EMERG "hwbp: init_kallsyms_lookup failed\n");
		return -EBADF;
	}
	printk(KERN_EMERG "hwbp: register_user_hw_breakpoint=%px unregister=%px\n",
		(void*)register_user_hw_breakpoint_sym, (void*)unregister_hw_breakpoint_sym);
#endif
	raw_spin_lock_init(&g_hwbp_lock);
	memset(g_hwbp_handles, 0, sizeof(g_hwbp_handles));

	g_hwBreakpointProc_devp = x_kmalloc(sizeof(struct hwBreakpointProcDev), GFP_KERNEL);
	if (!g_hwBreakpointProc_devp) {
		return -ENOMEM;
	}
	memset(g_hwBreakpointProc_devp, 0, sizeof(struct hwBreakpointProcDev));

#ifdef CONFIG_USE_PROC_FILE_NODE
	g_hwBreakpointProc_devp->proc_parent = proc_mkdir(CONFIG_PROC_NODE_AUTH_KEY, NULL);
	if(g_hwBreakpointProc_devp->proc_parent) {
		g_hwBreakpointProc_devp->proc_entry = proc_create(CONFIG_PROC_NODE_AUTH_KEY, S_IRUGO | S_IWUGO, g_hwBreakpointProc_devp->proc_parent, &hwBreakpointProc_proc_ops);
		/* hide_procfs_dir 的 kprobe（proc_root_readdir hook）在 6.12 有崩溃风险, 已禁用 */
		//start_hide_procfs_dir(CONFIG_PROC_NODE_AUTH_KEY);
	}
#endif

	printk(KERN_EMERG "hwbp: init ok (HIT_CAP=%d MAX_HANDLES=%d)\n",
		HWBP_HIT_CAP, HWBP_MAX_HANDLES);
	return 0;
}

static void hwBreakpointProc_dev_exit(void) {
	clean_hwbp();

#ifdef CONFIG_USE_PROC_FILE_NODE
	if(g_hwBreakpointProc_devp->proc_entry) {
		proc_remove(g_hwBreakpointProc_devp->proc_entry);
		g_hwBreakpointProc_devp->proc_entry = NULL;
	}

	if(g_hwBreakpointProc_devp->proc_parent) {
		proc_remove(g_hwBreakpointProc_devp->proc_parent);
		g_hwBreakpointProc_devp->proc_parent = NULL;
	}
	//stop_hide_procfs_dir();
#endif
	kfree(g_hwBreakpointProc_devp);
	printk(KERN_EMERG "hwbp: Goodbye\n");
}

int __init init_module(void) {
    return hwBreakpointProc_dev_init();
}

void __exit cleanup_module(void) {
    hwBreakpointProc_dev_exit();
}

#ifndef CONFIG_MODULE_GUIDE_ENTRY
//Hook:__cfi_check_fn
unsigned char* __check_(unsigned char* result, void *ptr, void *diag)
{
	printk_debug(KERN_EMERG "my__cfi_check_fn!!!\n");
	return result;
}

//Hook:__cfi_check_fail
unsigned char * __check_fail_(unsigned char *result)
{
	printk_debug(KERN_EMERG "my__cfi_check_fail!!!\n");
	return result;
}
#endif

unsigned long __stack_chk_guard;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
MODULE_DESCRIPTION("Linux default module");
