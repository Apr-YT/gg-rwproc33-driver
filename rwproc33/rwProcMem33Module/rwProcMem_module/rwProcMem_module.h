#ifndef RWPROCMEM_H_
#define RWPROCMEM_H_
#include <linux/module.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/version.h>

#include "api_proxy.h"
#include "phy_mem.h"
#include "proc_maps.h"
#include "proc_list.h"
#include "proc_root.h"
#include "proc_rss.h"
#include "proc_cmdline.h"
#include "ver_control.h"
#include "test.h"
#ifdef CONFIG_USE_PROC_FILE_NODE
#include <linux/proc_fs.h>
#include "hide_procfs_dir.h"
#endif
//////////////////////////////////////////////////////////////////

enum {
	CMD_INIT_DEVICE_INFO = 1, 		// 初始化设备信息
	CMD_OPEN_PROCESS, 				// 打开进程
	CMD_READ_PROCESS_MEMORY,       	// 读取进程内存
    CMD_WRITE_PROCESS_MEMORY,      	// 写入进程内存
	CMD_CLOSE_PROCESS, 				// 关闭进程
	CMD_GET_PROCESS_MAPS_COUNT, 	// 获取进程的内存块地址数量
	CMD_GET_PROCESS_MAPS_LIST, 		// 获取进程的内存块地址列表
	CMD_CHECK_PROCESS_ADDR_PHY,		// 检查进程内存是否有物理内存位置
	CMD_GET_PID_LIST,				// 获取进程PID列表
	CMD_SET_PROCESS_ROOT,			// 提升进程权限到Root
	CMD_GET_PROCESS_RSS,			// 获取进程的物理内存占用大小
	CMD_GET_PROCESS_CMDLINE_ADDR,	// 获取进程cmdline的内存地址
	CMD_HIDE_KERNEL_MODULE,			// 隐藏驱动
	CMD_GGTOUCH_TAP = 15,			// 内核虚拟触摸: 单点 tap（display 坐标）
	CMD_GGTOUCH_SWIPE = 16,			// 内核虚拟触摸: 滑动 swipe（display 坐标）
};

//////////////////////////////////////////////////////////////////
// 触摸注入参数（与用户态 agentd 共享的协议结构）
#pragma pack(push,1)
struct gg_touch_tap {
	uint32_t x;			// display 坐标 X（横屏 2608x1200 逻辑像素）
	uint32_t y;			// display 坐标 Y
	uint32_t hold_ms;	// 按下保持毫秒数（0 用默认 120ms）
};
struct gg_touch_swipe {
	uint32_t x1, y1;	// 起点 display 坐标
	uint32_t x2, y2;	// 终点 display 坐标
	uint32_t ms;		// 滑动总时长毫秒
};
#pragma pack(pop)

//////////////////////////////////////////////////////////////////
//rwProcMemDev设备结构体
struct rwProcMemDev {
#ifdef CONFIG_USE_PROC_FILE_NODE
	struct proc_dir_entry *proc_parent;
	struct proc_dir_entry *proc_entry;
#endif
	bool is_hidden_module; //是否已经隐藏过驱动列表了
};
static struct rwProcMemDev *g_rwProcMem_devp;

static ssize_t rwProcMem_read(struct file* filp, char __user* buf, size_t size, loff_t* ppos);
static const struct proc_ops rwProcMem_proc_ops = {
    .proc_read    = rwProcMem_read,
};

#endif /* RWPROCMEM_H_ */