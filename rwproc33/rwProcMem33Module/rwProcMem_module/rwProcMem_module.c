#include "rwProcMem_module.h"

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/delay.h>

#define MY_TASK_COMM_LEN 16

#pragma pack(push,1)
struct ioctl_request {
    char     cmd;        /* 1 字节命令 */
    uint64_t param1;     /* 参数1 */
    uint64_t param2;     /* 参数2 */
    uint64_t param3;     /* 参数3 */
    uint64_t buf_size;    /* 紧随其后的动态数据长度 */
};
struct init_device_info {
	int pid;
	int tgid;
	char my_name[MY_TASK_COMM_LEN + 1];
	char my_auxv[1024];
	int my_auxv_size;
};
struct arg_info {
	uint64_t arg_start;
	uint64_t arg_end;
};
#pragma pack(pop)

//////////////////////////////////////////////////////////////////////////
// 内核虚拟触摸（GG-TOUCH）
// 通过 input 子系统注册虚拟触摸设备，ioctl/read 命令触发 input_event()
// 注入，事件流与真实触屏一致：input core → evdev → InputReader → 应用。
// 坐标空间与 focaltech_ts 相同（raw 120000x260800 = display 1200x2608 的 100 倍）。
#define GG_TOUCH_DISP_W		2608	// 横屏 display 宽
#define GG_TOUCH_DISP_H		1200	// 横屏 display 高
#define GG_TOUCH_RAW_MAX_X	120000u	// raw X 上限（display 短边 1200 * 100）
#define GG_TOUCH_RAW_MAX_Y	260800u	// raw Y 上限（display 长边 2608 * 100）
#define GG_TOUCH_MAX_SLOTS	10		// 最大触点（与真实触屏一致）

static struct input_dev *gg_touch_dev = NULL;

// display(横屏 2608x1200) → raw(竖屏 1200x2608 * 100)：设备 CCW 旋转 90°
static void gg_disp2raw(uint32_t dx, uint32_t dy, uint32_t *rx, uint32_t *ry)
{
	if (dx >= GG_TOUCH_DISP_W) dx = GG_TOUCH_DISP_W - 1;
	if (dy >= GG_TOUCH_DISP_H) dy = GG_TOUCH_DISP_H - 1;
	*rx = GG_TOUCH_RAW_MAX_X - dy * 100u;
	*ry = dx * 100u;
}

// 单点 tap（raw 坐标）
static int gg_touch_tap_raw(uint32_t x, uint32_t y, uint32_t hold_ms)
{
	if (!gg_touch_dev)
		return -ENODEV;
	if (x >= GG_TOUCH_RAW_MAX_X) x = GG_TOUCH_RAW_MAX_X - 1;
	if (y >= GG_TOUCH_RAW_MAX_Y) y = GG_TOUCH_RAW_MAX_Y - 1;

	input_mt_slot(gg_touch_dev, 0);
	input_mt_report_slot_state(gg_touch_dev, MT_TOOL_FINGER, true);
	input_report_key(gg_touch_dev, BTN_TOUCH, 1);
	input_report_abs(gg_touch_dev, ABS_MT_POSITION_X, x);
	input_report_abs(gg_touch_dev, ABS_MT_POSITION_Y, y);
	input_sync(gg_touch_dev);

	if (hold_ms)
		msleep(hold_ms);

	input_mt_report_slot_state(gg_touch_dev, MT_TOOL_FINGER, false);
	input_report_key(gg_touch_dev, BTN_TOUCH, 0);
	input_sync(gg_touch_dev);
	return 0;
}

// 滑动（raw 坐标，线性插值）
static int gg_touch_swipe_raw(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t ms)
{
	const int steps = 20;
	int i;

	if (!gg_touch_dev)
		return -ENODEV;
	if (x1 >= GG_TOUCH_RAW_MAX_X) x1 = GG_TOUCH_RAW_MAX_X - 1;
	if (y1 >= GG_TOUCH_RAW_MAX_Y) y1 = GG_TOUCH_RAW_MAX_Y - 1;
	if (x2 >= GG_TOUCH_RAW_MAX_X) x2 = GG_TOUCH_RAW_MAX_X - 1;
	if (y2 >= GG_TOUCH_RAW_MAX_Y) y2 = GG_TOUCH_RAW_MAX_Y - 1;

	input_mt_slot(gg_touch_dev, 0);
	input_mt_report_slot_state(gg_touch_dev, MT_TOOL_FINGER, true);
	input_report_key(gg_touch_dev, BTN_TOUCH, 1);
	input_report_abs(gg_touch_dev, ABS_MT_POSITION_X, x1);
	input_report_abs(gg_touch_dev, ABS_MT_POSITION_Y, y1);
	input_sync(gg_touch_dev);

	for (i = 1; i <= steps; i++) {
		uint32_t cx = x1 + (uint32_t)(((uint64_t)(x2 - x1)) * i / steps);
		uint32_t cy = y1 + (uint32_t)(((uint64_t)(y2 - y1)) * i / steps);
		input_report_abs(gg_touch_dev, ABS_MT_POSITION_X, cx);
		input_report_abs(gg_touch_dev, ABS_MT_POSITION_Y, cy);
		input_sync(gg_touch_dev);
		msleep(ms / steps);
	}

	input_mt_report_slot_state(gg_touch_dev, MT_TOOL_FINGER, false);
	input_report_key(gg_touch_dev, BTN_TOUCH, 0);
	input_sync(gg_touch_dev);
	return 0;
}

static ssize_t OnCmdGgTouchTap(struct ioctl_request *hdr, char __user* buf) {
	struct gg_touch_tap arg;
	uint32_t rx, ry;

	if (hdr->buf_size != sizeof(arg))
		return -EINVAL;
	if (x_copy_from_user(&arg, buf, sizeof(arg)))
		return -EFAULT;

	gg_disp2raw(arg.x, arg.y, &rx, &ry);
	printk_debug(KERN_INFO "CMD_GGTOUCH_TAP disp(%u,%u) hold=%ums\n", arg.x, arg.y, arg.hold_ms);
	return gg_touch_tap_raw(rx, ry, arg.hold_ms ? arg.hold_ms : 120);
}

static ssize_t OnCmdGgTouchSwipe(struct ioctl_request *hdr, char __user* buf) {
	struct gg_touch_swipe arg;
	uint32_t rx1, ry1, rx2, ry2;

	if (hdr->buf_size != sizeof(arg))
		return -EINVAL;
	if (x_copy_from_user(&arg, buf, sizeof(arg)))
		return -EFAULT;

	gg_disp2raw(arg.x1, arg.y1, &rx1, &ry1);
	gg_disp2raw(arg.x2, arg.y2, &rx2, &ry2);
	printk_debug(KERN_INFO "CMD_GGTOUCH_SWIPE (%u,%u)->(%u,%u) %ums\n", arg.x1, arg.y1, arg.x2, arg.y2, arg.ms);
	return gg_touch_swipe_raw(rx1, ry1, rx2, ry2, arg.ms ? arg.ms : 300);
}

static int gg_touch_register(void)
{
	gg_touch_dev = input_allocate_device();
	if (!gg_touch_dev)
		return -ENOMEM;

	gg_touch_dev->name = "gg-ghost";
	gg_touch_dev->id.bustype = BUS_VIRTUAL;
	gg_touch_dev->id.vendor = 0x0666;
	gg_touch_dev->id.product = 0x0888;

	set_bit(EV_KEY, gg_touch_dev->evbit);
	set_bit(BTN_TOUCH, gg_touch_dev->keybit);
	set_bit(EV_ABS, gg_touch_dev->evbit);
	input_set_abs_params(gg_touch_dev, ABS_MT_POSITION_X, 0, GG_TOUCH_RAW_MAX_X - 1, 0, 0);
	input_set_abs_params(gg_touch_dev, ABS_MT_POSITION_Y, 0, GG_TOUCH_RAW_MAX_Y - 1, 0, 0);
	input_mt_init_slots(gg_touch_dev, GG_TOUCH_MAX_SLOTS, 0);

	if (input_register_device(gg_touch_dev)) {
		input_free_device(gg_touch_dev);
		gg_touch_dev = NULL;
		return -EIO;
	}
	printk(KERN_EMERG "gg-ghost: virtual input device registered\n");
	return 0;
}

static void gg_touch_unregister(void)
{
	if (gg_touch_dev) {
		input_unregister_device(gg_touch_dev);
		gg_touch_dev = NULL;
	}
}

static ssize_t OnCmdInitDeviceInfo(struct ioctl_request *hdr, char __user* buf) {
	long err = 0;
	struct init_device_info* pinit_device_info = (struct init_device_info*)x_kmalloc(sizeof(struct init_device_info), GFP_KERNEL);
	if (!pinit_device_info) {
		return -ENOMEM;
	}
	printk_debug(KERN_INFO "CMD_INIT_DEVICE_INFO\n");
	memset(pinit_device_info, 0, sizeof(struct init_device_info));
	if (x_copy_from_user((void*)pinit_device_info, (void*)buf, sizeof(struct init_device_info)) == 0) {
		printk_debug(KERN_INFO "my_name:%s\n", pinit_device_info->my_name);
		printk_debug(KERN_INFO "pid:%d, tgid:%d\n", pinit_device_info->pid, pinit_device_info->tgid);
		printk_debug(KERN_INFO "my_auxv_size:%d\n", pinit_device_info->my_auxv_size);
		
		do {
			err = init_mmap_lock_offset();
			if(err) { break; }
			err = init_map_count_offset();
			if(err) { break; }
			err = init_proc_cmdline_offset(&pinit_device_info->my_auxv[0], pinit_device_info->my_auxv_size);
			if(err) { break; }
			err = init_proc_root_offset(pinit_device_info->my_name);
			if(err) { break; }
			err = init_task_next_offset();
			if(err) { break; }
			err = init_task_pid_offset(pinit_device_info->pid, pinit_device_info->tgid);
		} while(0);
	} else {
		err = -EINVAL;
	}
	kfree(pinit_device_info);
	return err;
}

static ssize_t OnCmdOpenProcess(struct ioctl_request *hdr, char __user* buf) {
	uint64_t pid = hdr->param1, handle = 0;
	struct pid * proc_pid_struct = NULL;
	printk_debug(KERN_INFO "CMD_OPEN_PROCESS\n");

	printk_debug(KERN_INFO "pid:%llu,size:%ld\n", pid, sizeof(pid));

	proc_pid_struct = get_proc_pid_struct(pid);
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p\n", (void*)proc_pid_struct);
	if (!proc_pid_struct) {
		return -EINVAL;
	}
	handle = (uint64_t)proc_pid_struct;

	printk_debug(KERN_INFO "handle:%llu,size:%ld\n", handle, sizeof(handle));
	if (!!x_copy_to_user((void*)buf, (void*)&handle, sizeof(handle))) {
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdCloseProcess(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	printk_debug(KERN_INFO "CMD_CLOSE_PROCESS\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	release_proc_pid_struct(proc_pid_struct);
	return 0;
}

static ssize_t OnCmdReadProcessMemory(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	size_t proc_virt_addr = (size_t)hdr->param2;
	bool is_force_read = hdr->param3 == 1 ? true : false;
	size_t size = (size_t)hdr->buf_size;
	size_t read_size = 0;

	printk_debug(KERN_INFO "CMD_READ_PROCESS_MEMORY\n");

	printk_debug(KERN_INFO "READ proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));

	printk_debug(KERN_INFO "READ proc_virt_addr:0x%zx,size:%ld\n", proc_virt_addr, sizeof(proc_virt_addr));

	if (is_force_read == false && !check_proc_map_can_read(proc_pid_struct, proc_virt_addr, size)) {
		return -EFAULT;
	}

	while (read_size < size) {
		size_t phy_addr = 0;
		size_t pfn_sz = 0;
		char *lpOutBuf = NULL;
		pte_t *pte;

		bool old_pte_can_read;
		phy_addr = get_proc_phy_addr(proc_pid_struct, proc_virt_addr + read_size, &pte);
		printk_debug(KERN_INFO "calc phy_addr:0x%zx\n", phy_addr);

		if (phy_addr == 0) {
			break;
		}

		old_pte_can_read = is_pte_can_read(pte);
		if (is_force_read) {
			if (!old_pte_can_read) {
				if (!change_pte_read_status(pte, true)) { break; }

			}
		} else if (!old_pte_can_read) {
			break;
		} 

		pfn_sz = size_inside_page(phy_addr, ((size - read_size) > PAGE_SIZE) ? PAGE_SIZE : (size - read_size));
		printk_debug(KERN_INFO "pfn_sz:%zu\n", pfn_sz);


		lpOutBuf = (char*)(buf + read_size);
		read_ram_physical_addr(false, phy_addr, lpOutBuf, pfn_sz);

		if (is_force_read && old_pte_can_read == false) {
			change_pte_read_status(pte, false);
		}
		read_size += pfn_sz;
	}
	return read_size;
}

static ssize_t OnCmdWriteProcessMemory(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	size_t proc_virt_addr = (size_t)hdr->param2;
	bool is_force_write = hdr->param3 == 1 ? true : false;
	size_t size = (size_t)hdr->buf_size;
	size_t write_size = 0;
	printk_debug(KERN_INFO "CMD_WRITE_PROCESS_MEMORY\n");
	printk_debug(KERN_INFO "WRITE proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	printk_debug(KERN_INFO "WRITE proc_virt_addr:0x%zx,size:%ld\n", proc_virt_addr, sizeof(proc_virt_addr));

	if (is_force_write == false && !check_proc_map_can_write(proc_pid_struct, proc_virt_addr, size)) {
		return -EFAULT;
	}

	while (write_size < size) {
		size_t phy_addr = 0;
		size_t pfn_sz = 0;
		char * input_buf = NULL;

		pte_t *pte;
		bool old_pte_can_write;
		phy_addr = get_proc_phy_addr(proc_pid_struct, proc_virt_addr + write_size, &pte);
		printk_debug(KERN_INFO "phy_addr:0x%zx\n", phy_addr);
		if (phy_addr == 0) {
			break;
		}

		old_pte_can_write = is_pte_can_write(pte);
		if (is_force_write) {
			if (!old_pte_can_write) {
				if (!change_pte_write_status(pte, true)) { break; }
			}
		} else if (!old_pte_can_write) {
			break;
		}

		pfn_sz = size_inside_page(phy_addr, ((size - write_size) > PAGE_SIZE) ? PAGE_SIZE : (size - write_size));
		printk_debug(KERN_INFO "pfn_sz:%zu\n", pfn_sz);

		input_buf = (char*)(((size_t)buf + write_size));
		write_ram_physical_addr(phy_addr, input_buf, false, pfn_sz);
		if (is_force_write && old_pte_can_write == false) {
			change_pte_write_status(pte, false);
		}

		write_size += pfn_sz;
	}
	return write_size;
}

static ssize_t OnCmdGetProcessMapsCount(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	printk_debug(KERN_INFO "CMD_GET_PROCESS_MAPS_COUNT\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p, size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));

	return get_proc_map_count(proc_pid_struct);
}

static ssize_t OnCmdGetProcessMapsList(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	printk_debug(KERN_INFO "CMD_GET_PROCESS_MAPS_LIST\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	printk_debug(KERN_INFO "buf_size:%llu\n", hdr->buf_size);
	return get_proc_maps_list(false, proc_pid_struct, (void*)(buf), hdr->buf_size - 1);
}

static ssize_t OnCmdCheckProcessPhyAddr(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	size_t proc_virt_addr = (size_t)hdr->param2;
	pte_t *pte;
	printk_debug(KERN_INFO "CMD_CHECK_PROCESS_ADDR_PHY\n");
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	printk_debug(KERN_INFO "proc_virt_addr :0x%zx\n", proc_virt_addr);
	if (get_proc_phy_addr(proc_pid_struct, proc_virt_addr, &pte)) {
		return 1;
	}
	return 0;
}

static ssize_t OnCmdGetPidList(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_GET_PID_LIST\n");
	printk_debug(KERN_INFO "buf_size:%llu\n", hdr->buf_size);
	return get_proc_pid_list(false, buf, hdr->buf_size);
}

static ssize_t OnCmdSetProcessRoot(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	printk_debug(KERN_INFO "CMD_SET_PROCESS_ROOT\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	return set_process_root(proc_pid_struct);
}

static ssize_t OnCmdGetProcessRss(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	uint64_t rss = 0;
	printk_debug(KERN_INFO "CMD_GET_PROCESS_RSS\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	rss = read_proc_rss_size(proc_pid_struct);
	if (!!x_copy_to_user((void*)buf, &rss, sizeof(rss))) {
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdGetProcessCmdlineAddr(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = (struct pid *)hdr->param1;
	size_t arg_start = 0, arg_end = 0;
	int res;
	struct arg_info aginfo = {0};
	printk_debug(KERN_INFO "CMD_GET_PROCESS_CMDLINE_ADDR\n");
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	res = get_proc_cmdline_addr(proc_pid_struct, &arg_start, &arg_end);
	aginfo.arg_start = (uint64_t)arg_start;
	aginfo.arg_end = (uint64_t)arg_end;
	if (!!x_copy_to_user((void*)buf, &aginfo, sizeof(aginfo))) {
		return -EINVAL;
	}
	return res;
}

static ssize_t OnCmdHideKernelModule(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_HIDE_KERNEL_MODULE\n");

	if (g_rwProcMem_devp->is_hidden_module == false) {
		g_rwProcMem_devp->is_hidden_module = true; 
		list_del_init(&__this_module.list);
		kobject_del(&THIS_MODULE->mkobj.kobj);
	}
	return 0;
}

static inline ssize_t DispatchCommand(struct ioctl_request *hdr, char __user* buf) {
	switch (hdr->cmd) {
	case CMD_INIT_DEVICE_INFO:
		return OnCmdInitDeviceInfo(hdr, buf);
	case CMD_OPEN_PROCESS:
		return OnCmdOpenProcess(hdr, buf);
	case CMD_READ_PROCESS_MEMORY:
		return OnCmdReadProcessMemory(hdr, buf);
	case CMD_WRITE_PROCESS_MEMORY:
		return OnCmdWriteProcessMemory(hdr, buf);
	case CMD_CLOSE_PROCESS:
		return OnCmdCloseProcess(hdr, buf);
	case CMD_GET_PROCESS_MAPS_COUNT:
		return OnCmdGetProcessMapsCount(hdr, buf);
	case CMD_GET_PROCESS_MAPS_LIST:
		return OnCmdGetProcessMapsList(hdr, buf);
	case CMD_CHECK_PROCESS_ADDR_PHY:
		return OnCmdCheckProcessPhyAddr(hdr, buf);
	case CMD_GET_PID_LIST:
		return OnCmdGetPidList(hdr, buf);
	case CMD_SET_PROCESS_ROOT:
		return OnCmdSetProcessRoot(hdr, buf);
	case CMD_GET_PROCESS_RSS:
		return OnCmdGetProcessRss(hdr, buf);
	case CMD_GET_PROCESS_CMDLINE_ADDR:
		return OnCmdGetProcessCmdlineAddr(hdr, buf);
	case CMD_HIDE_KERNEL_MODULE:
		return OnCmdHideKernelModule(hdr, buf);
	case CMD_GGTOUCH_TAP:
		return OnCmdGgTouchTap(hdr, buf);
	case CMD_GGTOUCH_SWIPE:
		return OnCmdGgTouchSwipe(hdr, buf);
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

static ssize_t rwProcMem_read(struct file* filp,
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

static int rwProcMem_dev_init(void) {
	int err;

	err = gg_touch_register();
	if (err) {
		printk(KERN_EMERG "gg-touch: register failed (%d)\n", err);
		return err;
	}

	g_rwProcMem_devp = x_kmalloc(sizeof(struct rwProcMemDev), GFP_KERNEL);
	memset(g_rwProcMem_devp, 0, sizeof(struct rwProcMemDev));

#ifdef CONFIG_USE_PROC_FILE_NODE
	g_rwProcMem_devp->proc_parent = proc_mkdir(CONFIG_PROC_NODE_AUTH_KEY, NULL);
	if(g_rwProcMem_devp->proc_parent) {
		g_rwProcMem_devp->proc_entry = proc_create(CONFIG_PROC_NODE_AUTH_KEY, S_IRUGO | S_IWUGO, g_rwProcMem_devp->proc_parent, &rwProcMem_proc_ops);
		start_hide_procfs_dir(CONFIG_PROC_NODE_AUTH_KEY);
	}
#endif

#ifdef CONFIG_DEBUG_PRINTK
	printk(KERN_EMERG "Hello, %s debug\n", CONFIG_PROC_NODE_AUTH_KEY);
	//test1();
	//test2();
	//test3();
	//test4();
	//test5();
#else
	printk(KERN_EMERG "Hello\n");
#endif
	return 0;
}

static void rwProcMem_dev_exit(void) {
#ifdef CONFIG_USE_PROC_FILE_NODE
	if(g_rwProcMem_devp->proc_entry) {
		proc_remove(g_rwProcMem_devp->proc_entry);
		g_rwProcMem_devp->proc_entry = NULL;
	}
	
	if(g_rwProcMem_devp->proc_parent) {
		proc_remove(g_rwProcMem_devp->proc_parent);
		g_rwProcMem_devp->proc_parent = NULL;
	}
	stop_hide_procfs_dir();
#endif
	gg_touch_unregister();
	kfree(g_rwProcMem_devp);
	printk(KERN_EMERG "Goodbye\n");
}

int __init init_module(void) {
    return rwProcMem_dev_init();
}

void __exit cleanup_module(void) {
    rwProcMem_dev_exit();
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

