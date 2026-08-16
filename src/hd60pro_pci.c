// SPDX-License-Identifier: GPL-2.0-only
/*
 * Elgato HD60 Pro / YUAN 12ab:0380 experimental PCI bring-up driver.
 *
 * This module intentionally performs no capture yet. It binds the PCI device,
 * maps BARs, and exposes opt-in diagnostics so the register model can be built
 * from observed hardware behavior instead of guesses.
 */

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define HD60PRO_VENDOR_ID		0x12ab
#define HD60PRO_DEVICE_ID		0x0380
#define HD60PRO_SUBVENDOR_ELGATO	0x1cfa
#define HD60PRO_SUBDEVICE_HD60PRO_0003	0x0003
#define HD60PRO_SUBDEVICE_HD60PRO_0005	0x0005
#define HD60PRO_SUBDEVICE_HD60PRO_0006	0x0006

#define HD60PRO_BAR0			0
#define HD60PRO_BAR5			5
#define HD60PRO_DUMP_BYTES		0x100
#define HD60PRO_BAR5_REAL_BYTES		0x200
#define HD60PRO_CONFIG_DUMP_BYTES	0x100
#define HD60PRO_DEFAULT_WIDTH		1920
#define HD60PRO_DEFAULT_HEIGHT		1080
#define HD60PRO_DEFAULT_FIELD		V4L2_FIELD_NONE
#define HD60PRO_DEFAULT_COLORSPACE	V4L2_COLORSPACE_REC709
#define HD60PRO_DEFAULT_PIXELCLOCK	148500000ULL
#define HD60PRO_DEFAULT_FRAME_PERIOD_NS	(NSEC_PER_SEC / 60)

#define HD60PRO_REG_DOORBELL		0x000
#define HD60PRO_REG_MBOX_COMPLETE	0x02c
#define HD60PRO_REG_IRQ_STATUS		0x030
#define HD60PRO_REG_IRQ_ACK_SIDEBAND	0x0dc
#define HD60PRO_REG_IRQ_ACK_DOORBELL	0x400
#define HD60PRO_IRQ_MBOX_COMPLETE	BIT(11)
#define HD60PRO_MBOX_DOORBELL		0x800
#define HD60PRO_MBOX_CMD_PREINIT	0x01
#define HD60PRO_MBOX_CMD_FW_STATUS	0x0a
#define HD60PRO_MBOX_CMD_GPIO_SET	0x17
#define HD60PRO_MBOX_CMD_GPIO_ALT_SET	0x15
#define HD60PRO_MBOX_CMD_I2C_READ8	0x1a
#define HD60PRO_MBOX_CMD_PIPELINE_READ	HD60PRO_MBOX_CMD_I2C_READ8
#define HD60PRO_MBOX_CMD_PIPELINE_WRITE	0x1b
#define HD60PRO_MBOX_CMD_SELECTOR_READ	0x1e
#define HD60PRO_MBOX_CMD_I2C_WRITE_EXT	0x1d
#define HD60PRO_MBOX_CMD_LOGO_PREPARE	0x60
#define HD60PRO_MBOX_CMD_LOGO_COMMIT	0x61
#define HD60PRO_MBOX_CMD_DOWNLOAD_FW_PREPARE	0x0b
#define HD60PRO_MBOX_CMD_DOWNLOAD_FW_COMMIT	0x0c
#define HD60PRO_MBOX_CMD_GET_VERSION	0x1c
#define HD60PRO_MBOX_SELECTOR_FW_A3	0xa3
#define HD60PRO_MBOX_POLL_US		1000
#define HD60PRO_MBOX_TIMEOUT_US		500000
#define HD60PRO_MBOX_ASYNC_TIMEOUT_MS	5000
#define HD60PRO_VISIBLE_FW_COMMIT_TIMEOUT_MS	60000
#define HD60PRO_FW_WINDOW_OFFSET	0x60
#define HD60PRO_LOGO_PAYLOAD_SIZE	0x25800
#define HD60PRO_LOGO_DESCRIPTOR		0x00f00140
#define HD60PRO_DMA_DESC_BYTES		0x1000
#define HD60PRO_HOST_DESC_MAGIC		0x48503630 /* "HP60" */
#define HD60PRO_HOST_DESC_VERSION	1
#define HD60PRO_WINDOWS_STREAM_FPS_60	0x0000bb80
#define HD60PRO_EP_CMD_SET_VIC_PARAMS	0x29
#define HD60PRO_EP_CMD_POST_SET_VIC	0x2a
#define HD60PRO_EP_SET_VIC_PAYLOAD_BYTES 0x28
#define HD60PRO_EP_EVENT_RECORD_BYTES	0x2c
#define HD60PRO_WINDOWS_DMA_CONTROL_BYTES 0x1000
#define HD60PRO_WINDOWS_DMA_STATUS_BYTES 0x1000
#define HD60PRO_WINDOWS_DMA_CHANNELS	4
#define HD60PRO_WINDOWS_DMA_CHANNEL_BYTES 0x1000

enum hd60pro_board_id {
	HD60PRO_BOARD_UNKNOWN,
	HD60PRO_BOARD_0003,
	HD60PRO_BOARD_0005,
	HD60PRO_BOARD_0006,
};

/*
 * Working model for the Windows DirectMemory/frame helper at 0x14027eb38.
 * These offsets are not a Linux userspace ABI; they document the Windows
 * per-buffer status structure while the real endpoint/DMA producer is decoded.
 */
struct hd60pro_direct_frame_meta {
	u64 timestamp_ns;	/* Windows status+0x08 equivalent */
	u64 duration_ns;	/* Windows status+0x18 equivalent */
	u32 payload_bytes;	/* Windows status+0x24 equivalent */
	u32 flags;		/* Windows status+0x30 equivalent */
	u32 extra;		/* Windows status+0x38 presence/status equivalent */
	u32 sequence;
};

/*
 * Candidate host-visible descriptor. This is a Linux-side scaffold only. The
 * endpoint advertisement format is still unknown, so hardware never receives
 * this address until the Windows hready/epint/channel_done protocol is mapped.
 */
struct hd60pro_host_frame_desc {
	__le32 magic;
	__le32 version;
	__le64 frame_dma;
	__le32 frame_bytes;
	__le32 width;
	__le32 height;
	__le32 fourcc;
	__le32 fps_millihz;
	__le32 windows_stream_8144;
	__le32 windows_stream_8148;
	__le32 windows_stream_814c;
	__le32 windows_stream_8578;
	__le32 windows_stream_72c0;
	__le32 meta_size;
	__le32 reserved[18];
};

/*
 * Firmware endpoint command 0x29 model from ep.ko:pciep_isr. This mirrors the
 * firmware-side ep_command offsets only; Linux does not submit it until the
 * host-to-endpoint transport is decoded.
 */
struct hd60pro_ep_set_vic_model {
	u8 command_id;		/* ep_command+0x00, observed ISR command 0x29 */
	u8 flags0;		/* ep_command+0x04, zero prints SET_VIC log */
	u8 fps;			/* ep_command+0x05 */
	u8 fw_or_mode;		/* ep_command+0x06, value 7 selects 1080p epint */
	__le16 width;		/* ep_command+0x08 */
	__le16 height;		/* ep_command+0x0a */
	u8 interrupt_reduce;	/* ep_command+0x22 */
};

static const u8 hd60pro_ep_cmds_size[35] = {
	[24] = 0x0c, [25] = 0x0c, [26] = 0x10, [27] = 0x10,
	[28] = 0x10, [29] = 0x14, [30] = 0x2c, [31] = 0x2c,
	[32] = 0x2c, [33] = 0x2c, [34] = 0x2c,
};

static const u8 hd60pro_ep_ints_size[111] = {
	[6] = 0x08, [7] = 0x08, [9] = 0x08, [41] = 0x28,
	[42] = 0x14, [45] = 0x2c, [47] = 0x2c, [49] = 0x14,
	[80] = 0x2c, [81] = 0x14, [82] = 0x07, [96] = 0x10,
	[97] = 0x08, [98] = 0x0c, [110] = 0x18,
};

static const u8 hd60pro_ep_ints_1080p_size[99] = {
	[6] = 0x08, [7] = 0x08, [9] = 0x08, [41] = 0x28,
	[42] = 0x14, [45] = 0x2c, [47] = 0x2c, [49] = 0x14,
	[80] = 0x2c, [81] = 0x14, [82] = 0x07, [98] = 0x0c,
};

static const u8 hd60pro_ep_aic_size[44] = {
	[6] = 0x08, [7] = 0x08, [42] = 0x14, [43] = 0x14,
};

struct hd60pro_board {
	enum hd60pro_board_id id;
	const char *name;
};

static const struct hd60pro_board hd60pro_board_0003 = {
	.id = HD60PRO_BOARD_0003,
	.name = "Elgato Game Capture HD60 Pro subsystem 1cfa:0003",
};

static const struct hd60pro_board hd60pro_board_0005 = {
	.id = HD60PRO_BOARD_0005,
	.name = "Elgato Game Capture HD60 Pro subsystem 1cfa:0005",
};

static const struct hd60pro_board hd60pro_board_0006 = {
	.id = HD60PRO_BOARD_0006,
	.name = "Elgato Game Capture HD60 Pro subsystem 1cfa:0006",
};

static bool mmio_dump;
module_param(mmio_dump, bool, 0444);
MODULE_PARM_DESC(mmio_dump, "Allow debugfs reads of the first 256 bytes of mapped BARs");

static bool enable_busmaster;
module_param(enable_busmaster, bool, 0444);
MODULE_PARM_DESC(enable_busmaster, "Enable PCI bus mastering during probe; keep off until DMA is understood");

static bool request_irq_vector;
module_param(request_irq_vector, bool, 0444);
MODULE_PARM_DESC(request_irq_vector, "Request one IRQ vector for diagnostics; keep off until interrupt ack registers are known");

static bool enable_v4l2;
module_param(enable_v4l2, bool, 0444);
MODULE_PARM_DESC(enable_v4l2, "Register a diagnostic V4L2 node without streaming support");

static bool allow_mailbox_writes;
module_param(allow_mailbox_writes, bool, 0444);
MODULE_PARM_DESC(allow_mailbox_writes, "Allow experimental mailbox writes from root-only debugfs files");

static bool allow_firmware_load;
module_param(allow_firmware_load, bool, 0444);
MODULE_PARM_DESC(allow_firmware_load, "Allow experimental firmware download sequence from root-only debugfs files");

static bool allow_preinit_command1;
module_param(allow_preinit_command1, bool, 0444);
MODULE_PARM_DESC(allow_preinit_command1, "Allow experimental Windows pre-init mailbox command 0x01");

static bool allow_fw_status_command10;
module_param(allow_fw_status_command10, bool, 0444);
MODULE_PARM_DESC(allow_fw_status_command10, "Allow experimental Windows firmware status mailbox command 0x0a");

static bool allow_gpio_command17;
module_param(allow_gpio_command17, bool, 0444);
MODULE_PARM_DESC(allow_gpio_command17, "Allow experimental Windows GPIO mailbox command 0x17");

static bool allow_gpio17_sequence;
module_param(allow_gpio17_sequence, bool, 0444);
MODULE_PARM_DESC(allow_gpio17_sequence, "Allow experimental Windows generic GPIO command 0x17 sequence");

static bool allow_i2c_read_command1a;
module_param(allow_i2c_read_command1a, bool, 0444);
MODULE_PARM_DESC(allow_i2c_read_command1a, "Allow experimental Windows I2C-like read mailbox command 0x1a");

static bool allow_logo_upload;
module_param(allow_logo_upload, bool, 0444);
MODULE_PARM_DESC(allow_logo_upload, "Allow experimental Windows logo/status payload upload commands 0x60/0x61");

static bool allow_cmd1d_write;
module_param(allow_cmd1d_write, bool, 0444);
MODULE_PARM_DESC(allow_cmd1d_write, "Allow experimental Windows command 0x1d byte writes after logo upload");

static bool allow_post_logo_pipeline;
module_param(allow_post_logo_pipeline, bool, 0444);
MODULE_PARM_DESC(allow_post_logo_pipeline, "Allow experimental minimal Windows post-logo pipeline command 0x1b sequence");

static bool allow_capture_88_writes;
module_param(allow_capture_88_writes, bool, 0444);
MODULE_PARM_DESC(allow_capture_88_writes, "Allow experimental Windows capture chip 0x88 preset writes after pipeline-ready init");

static bool allow_unsafe_visible_fw_prepare;
module_param(allow_unsafe_visible_fw_prepare, bool, 0444);
MODULE_PARM_DESC(allow_unsafe_visible_fw_prepare, "Allow known-unsafe cold visible firmware prepare command 0x0b");

static bool auto_init;
module_param(auto_init, bool, 0444);
MODULE_PARM_DESC(auto_init, "Run the validated Windows-style mailbox init during probe when all required opt-in gates are enabled");

static bool prepare_dma_buffers;
module_param(prepare_dma_buffers, bool, 0444);
MODULE_PARM_DESC(prepare_dma_buffers, "Allocate coherent diagnostic DMA descriptor/frame buffers without starting bus mastering");

static bool synthetic_v4l2 = true;
module_param(synthetic_v4l2, bool, 0444);
MODULE_PARM_DESC(synthetic_v4l2, "Complete V4L2 buffers with black YUYV frames until real capture DMA is decoded");

static int mailbox_bar = HD60PRO_BAR0;
module_param(mailbox_bar, int, 0444);
MODULE_PARM_DESC(mailbox_bar, "BAR used for Windows-style mailbox commands; Windows mapping analysis points at BAR0");

static char *firmware_name = "hd60prodrv/MZ0380.HD.HEX";
module_param(firmware_name, charp, 0444);
MODULE_PARM_DESC(firmware_name, "Firmware file name under /lib/firmware for future firmware-load experiments");

struct hd60pro_dev {
	struct pci_dev *pdev;
	const struct hd60pro_board *board;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vb2q;
	struct mutex video_lock;
	spinlock_t queued_lock;
	struct list_head queued_bufs;
	bool pipeline_ready;
	bool streaming;
	u32 sequence;
	struct hd60pro_direct_frame_meta last_frame_meta;
	u32 direct_memory_blob[4];
	void *dma_desc_cpu;
	dma_addr_t dma_desc_dma;
	size_t dma_desc_size;
	void *dma_frame_cpu;
	dma_addr_t dma_frame_dma;
	size_t dma_frame_size;
	void *win_dma_control_cpu;
	dma_addr_t win_dma_control_dma;
	size_t win_dma_control_size;
	void *win_dma_status_cpu;
	dma_addr_t win_dma_status_dma;
	size_t win_dma_status_size;
	void *win_dma_channel_cpu[HD60PRO_WINDOWS_DMA_CHANNELS];
	dma_addr_t win_dma_channel_dma[HD60PRO_WINDOWS_DMA_CHANNELS];
	size_t win_dma_channel_size[HD60PRO_WINDOWS_DMA_CHANNELS];
	void __iomem *bar0;
	void __iomem *bar5;
	resource_size_t bar0_len;
	resource_size_t bar5_len;
	int irq;
	struct mutex mailbox_lock;
	u32 last_irq_status;
	u32 irq_count;
	u32 mailbox_irq_count;
	struct dentry *debugfs_dir;
};

static struct dentry *hd60pro_debugfs_root;

static void __iomem *hd60pro_mailbox_base(struct hd60pro_dev *hd)
{
	if (mailbox_bar == HD60PRO_BAR5)
		return hd->bar5;

	return hd->bar0;
}

static resource_size_t hd60pro_mailbox_len(struct hd60pro_dev *hd)
{
	if (mailbox_bar == HD60PRO_BAR5)
		return hd->bar5_len;

	return hd->bar0_len;
}

static const char *hd60pro_mailbox_bar_name(void)
{
	if (mailbox_bar == HD60PRO_BAR5)
		return "bar5";
	if (mailbox_bar == HD60PRO_BAR0)
		return "bar0";

	return "invalid";
}

static const struct pci_device_id hd60pro_pci_ids[] = {
	{
		PCI_DEVICE_SUB(HD60PRO_VENDOR_ID, HD60PRO_DEVICE_ID,
			       HD60PRO_SUBVENDOR_ELGATO,
			       HD60PRO_SUBDEVICE_HD60PRO_0006),
		.driver_data = (kernel_ulong_t)&hd60pro_board_0006,
	},
	{
		PCI_DEVICE_SUB(HD60PRO_VENDOR_ID, HD60PRO_DEVICE_ID,
			       HD60PRO_SUBVENDOR_ELGATO,
			       HD60PRO_SUBDEVICE_HD60PRO_0005),
		.driver_data = (kernel_ulong_t)&hd60pro_board_0005,
	},
	{
		PCI_DEVICE_SUB(HD60PRO_VENDOR_ID, HD60PRO_DEVICE_ID,
			       HD60PRO_SUBVENDOR_ELGATO,
			       HD60PRO_SUBDEVICE_HD60PRO_0003),
		.driver_data = (kernel_ulong_t)&hd60pro_board_0003,
	},
	{ }
};
MODULE_DEVICE_TABLE(pci, hd60pro_pci_ids);

static unsigned int hd60pro_frame_size(void);

static irqreturn_t hd60pro_irq(int irq, void *data)
{
	struct hd60pro_dev *hd = data;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 status;

	if (!base || !hd->bar5)
		return IRQ_NONE;

	status = ioread32(base + HD60PRO_REG_IRQ_STATUS);
	if (!status || status == U32_MAX)
		return IRQ_NONE;

	hd->last_irq_status = status;
	hd->irq_count++;
	if (status & HD60PRO_IRQ_MBOX_COMPLETE)
		hd->mailbox_irq_count++;

	/*
	 * The Windows driver IRQ path reads BAR0+0x30, writes BAR5+0xdc = 2,
	 * writes BAR0+0x30 = 0, then rings BAR0+0x00 with 0x400. We keep the
	 * acknowledged sideband write because device+0x110 maps to BAR5 here,
	 * and acknowledge the observed BAR5 status as a conservative fallback.
	 */
	iowrite32(2, hd->bar5 + HD60PRO_REG_IRQ_ACK_SIDEBAND);
	iowrite32(0, base + HD60PRO_REG_IRQ_STATUS);
	iowrite32(HD60PRO_REG_IRQ_ACK_DOORBELL,
		  base + HD60PRO_REG_DOORBELL);

	dev_dbg_ratelimited(&hd->pdev->dev, "irq status=0x%08x\n", status);
	return IRQ_HANDLED;
}

static bool hd60pro_mailbox_dead(void __iomem *base)
{
	return ioread32(base + 0x008) == U32_MAX &&
	       ioread32(base + 0x00c) == U32_MAX &&
	       ioread32(base + 0x02c) == U32_MAX &&
	       ioread32(base + 0x030) == U32_MAX;
}

static int hd60pro_mailbox_send_locked(struct hd60pro_dev *hd,
				       const u32 *packet, unsigned int dwords,
				       unsigned int timeout_us,
				       u32 *completion)
{
	u32 done;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int ret;

	if (!allow_mailbox_writes)
		return -EPERM;
	if (!base || dwords < 2)
		return -ENODEV;

	if (hd60pro_mailbox_dead(base))
		return -ENODEV;

	iowrite32(0, base + HD60PRO_REG_MBOX_COMPLETE);
	for (i = 1; i < dwords; i++)
		iowrite32(packet[i], base + i * sizeof(u32));
	iowrite32(packet[0], base + HD60PRO_REG_DOORBELL);

	ret = readl_poll_timeout(base + HD60PRO_REG_MBOX_COMPLETE, done,
				 done != U32_MAX && (done & BIT(0)),
				 HD60PRO_MBOX_POLL_US,
				 timeout_us);
	if (!ret && done == U32_MAX)
		ret = -ENODEV;
	if (completion)
		*completion = done;

	return ret;
}

static int hd60pro_mailbox_send3(struct hd60pro_dev *hd, u32 command,
				 u32 arg0, u32 *completion)
{
	const u32 packet[] = {
		HD60PRO_MBOX_DOORBELL,
		command,
		arg0,
	};
	int ret;

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  completion);
	mutex_unlock(&hd->mailbox_lock);

	return ret;
}

static int hd60pro_mailbox_send_async_locked(struct hd60pro_dev *hd,
					     const u32 *packet,
					     unsigned int dwords,
					     unsigned int timeout_ms,
					     u32 *completion,
					     u32 *irq_delta)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 irq_before = hd->irq_count;
	u32 done = 0;
	unsigned int waited;
	unsigned int i;

	if (!allow_mailbox_writes)
		return -EPERM;
	if (!base || dwords < 2)
		return -ENODEV;
	if (hd60pro_mailbox_dead(base))
		return -ENODEV;

	for (i = 1; i < dwords; i++)
		iowrite32(packet[i], base + i * sizeof(u32));
	iowrite32(packet[0], base + HD60PRO_REG_DOORBELL);

	for (waited = 0; waited < timeout_ms; waited++) {
		done = ioread32(base + HD60PRO_REG_MBOX_COMPLETE);
		if (done == U32_MAX)
			return -ENODEV;
		if (done & BIT(0))
			break;
		if (hd->irq_count != irq_before)
			break;
		msleep(1);
	}

	if (completion)
		*completion = done;
	if (irq_delta)
		*irq_delta = hd->irq_count - irq_before;

	if (waited >= timeout_ms)
		return -ETIMEDOUT;

	return 0;
}

static int hd60pro_mailbox_send_logo_prepare_locked(struct hd60pro_dev *hd,
						    const u32 *packet,
						    unsigned int dwords,
						    unsigned int timeout_ms,
						    u32 *bar5_window_start,
						    u32 *bar5_window_end)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 expected_start = (u32)pci_resource_start(hd->pdev, HD60PRO_BAR0) +
			     HD60PRO_FW_WINDOW_OFFSET;
	u32 expected_end = expected_start + (HD60PRO_LOGO_PAYLOAD_SIZE * 3) - 1;
	unsigned int waited;
	unsigned int i;
	u32 start = 0;
	u32 end = 0;

	if (!allow_mailbox_writes)
		return -EPERM;
	if (!base || !hd->bar5 || dwords < 2)
		return -ENODEV;
	if (hd60pro_mailbox_dead(base))
		return -ENODEV;

	for (i = 1; i < dwords; i++)
		iowrite32(packet[i], base + i * sizeof(u32));
	iowrite32(packet[0], base + HD60PRO_REG_DOORBELL);

	for (waited = 0; waited < timeout_ms; waited++) {
		start = ioread32(hd->bar5 + 0x040);
		end = ioread32(hd->bar5 + 0x048);
		if (start == U32_MAX && end == U32_MAX)
			return -ENODEV;
		if (start == expected_start && end == expected_end)
			break;
		msleep(1);
	}

	if (bar5_window_start)
		*bar5_window_start = start;
	if (bar5_window_end)
		*bar5_window_end = end;

	if (waited >= timeout_ms)
		return -ETIMEDOUT;

	return 0;
}

static u32 hd60pro_windows_challenge_hash(const u8 bytes[4])
{
	static const u32 constants[] = {
		0xa539c75a,
		0x9f28a543,
		0x7b6324c5,
		0xf1029554,
	};
	u32 value = 0x12345678;
	u32 constant = constants[bytes[0] & 3];
	unsigned int i;

	for (i = 0; i < 4; i++) {
		value = ((value << 8) ^ bytes[i]) ^ constant;
		value ^= (value >> 8) | (value << 24);
	}

	return value;
}

static int hd60pro_fw_version_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 completion = 0;
	u32 response0;
	u32 response1;
	u32 response2;
	void __iomem *base = hd60pro_mailbox_base(hd);
	int ret;

	if (!allow_mailbox_writes) {
		seq_printf(s,
			   "disabled; reload with allow_mailbox_writes=1 mailbox_bar=%d to issue mailbox command 0x1c selector 0xa3\n",
			   mailbox_bar);
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	ret = hd60pro_mailbox_send3(hd, HD60PRO_MBOX_CMD_GET_VERSION,
				    HD60PRO_MBOX_SELECTOR_FW_A3, &completion);
	response0 = ioread32(base + 0x008);
	response1 = ioread32(base + 0x00c);
	response2 = ioread32(base + 0x010);

	seq_puts(s, "command: get_firmware_version_windows\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "packet: 0x%08x 0x%08x 0x%08x\n",
		   HD60PRO_MBOX_DOORBELL, HD60PRO_MBOX_CMD_GET_VERSION,
		   HD60PRO_MBOX_SELECTOR_FW_A3);
	seq_printf(s, "result: %d\n", ret);
	seq_printf(s, "completion: 0x%08x\n", completion);
	seq_printf(s, "response_arg0_%s_0x08: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), response0);
	seq_printf(s, "response_arg1_%s_0x0c: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), response1);
	seq_printf(s, "response_version_%s_0x10: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), response2);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_fw_version);

static int hd60pro_preinit_command1_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[2] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PREINIT,
	};
	u32 completion = 0;
	u32 irq_delta = 0;
	u32 bar5_ref0;
	u32 bar5_ref1;
	u32 mbox_irq_status;
	int ret;

	seq_puts(s, "windows_preinit: command 0x01 async path before visible firmware download\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	bar5_ref0 = hd->bar5 ? ioread32(hd->bar5 + 0x030) : U32_MAX;
	bar5_ref1 = hd->bar5 ? ioread32(hd->bar5 + 0x038) : U32_MAX;
	mbox_irq_status = base ? ioread32(base + HD60PRO_REG_IRQ_STATUS) : U32_MAX;
	seq_printf(s, "bar5_030_ref_before: 0x%08x\n", bar5_ref0);
	seq_printf(s, "bar5_038_ref_before: 0x%08x\n", bar5_ref1);
	seq_printf(s, "mailbox_030_irq_status_before: 0x%08x\n", mbox_irq_status);
	seq_printf(s, "irq_count_before: %u\n", hd->irq_count);

	if (!allow_preinit_command1) {
		seq_puts(s, "blocked; reload with allow_preinit_command1=1 allow_mailbox_writes=1 request_irq_vector=1 to send this command\n");
		seq_puts(s, "planned writes: BAR5+0xdc=2, mailbox+0x30=0, mailbox+0x00=0x400, BAR5+0x30=BAR0+4, BAR5+0x38=BAR0+0x5f, packet=[0x800,0x01]\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (hd->irq < 0) {
		seq_puts(s, "disabled; request_irq_vector=1 is required for the async Windows path\n");
		return 0;
	}

	if (!base || !hd->bar5) {
		seq_puts(s, "selected mailbox BAR or BAR5 is not mapped\n");
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	iowrite32(2, hd->bar5 + HD60PRO_REG_IRQ_ACK_SIDEBAND);
	iowrite32(0, base + HD60PRO_REG_IRQ_STATUS);
	iowrite32(HD60PRO_REG_IRQ_ACK_DOORBELL, base + HD60PRO_REG_DOORBELL);
	iowrite32((u32)pci_resource_start(hd->pdev, HD60PRO_BAR0) + 0x4,
		  hd->bar5 + 0x030);
	iowrite32((u32)pci_resource_start(hd->pdev, HD60PRO_BAR0) +
		  HD60PRO_FW_WINDOW_OFFSET - 0x1, hd->bar5 + 0x038);
	ret = hd60pro_mailbox_send_async_locked(hd, packet, ARRAY_SIZE(packet),
						5000, &completion, &irq_delta);
	mbox_irq_status = ioread32(base + HD60PRO_REG_IRQ_STATUS);
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "packet: 0x%08x 0x%08x\n", packet[0], packet[1]);
	seq_printf(s, "result: %d\n", ret);
	seq_printf(s, "completion: 0x%08x\n", completion);
	seq_printf(s, "irq_delta: %u\n", irq_delta);
	seq_printf(s, "irq_count_after: %u\n", hd->irq_count);
	seq_printf(s, "mailbox_030_irq_status_after: 0x%08x\n",
		   mbox_irq_status);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_preinit_command1);

static int hd60pro_fw_status_command10_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[4] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_FW_STATUS,
		0,
		0,
	};
	u32 completion = 0;
	u32 irq_delta = 0;
	u32 status0;
	u32 status1;
	u32 status2;
	u32 marker = 0;
	unsigned int waited_ms;
	int ret;

	seq_puts(s, "windows_fw_status: command 0x0a async path before visible firmware selection\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_fw_status_command10) {
		seq_puts(s, "blocked; run preinit_command1 first, then reload with allow_fw_status_command10=1 allow_mailbox_writes=1 request_irq_vector=1 to send this command\n");
		seq_puts(s, "planned packet: [0x800,0x0a,0,0], Windows then checks BAR0+0x2c for 0xaaaaaaaa and BAR0+0x08/0x0c against MZ0380.FW.TXT\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (hd->irq < 0) {
		seq_puts(s, "disabled; request_irq_vector=1 is required for the async Windows path\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_async_locked(hd, packet, ARRAY_SIZE(packet),
						HD60PRO_MBOX_ASYNC_TIMEOUT_MS,
						&completion, &irq_delta);
	for (waited_ms = 0; waited_ms < HD60PRO_MBOX_ASYNC_TIMEOUT_MS; waited_ms++) {
		marker = ioread32(base + HD60PRO_REG_MBOX_COMPLETE);
		if (marker == 0xaaaaaaaa || marker == U32_MAX)
			break;
		msleep(1);
	}
	status0 = ioread32(base + 0x008);
	status1 = ioread32(base + 0x00c);
	status2 = ioread32(base + 0x010);
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "packet: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   packet[0], packet[1], packet[2], packet[3]);
	seq_printf(s, "result: %d\n", ret);
	seq_printf(s, "completion: 0x%08x\n", completion);
	seq_printf(s, "windows_marker_after_wait: 0x%08x\n", marker);
	seq_printf(s, "windows_marker_wait_ms: %u\n", waited_ms);
	seq_printf(s, "completion_matches_windows_marker: %d\n",
		   marker == 0xaaaaaaaa);
	seq_printf(s, "irq_delta: %u\n", irq_delta);
	seq_printf(s, "status_bar_%s_0x08: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status0);
	seq_printf(s, "status_bar_%s_0x0c: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status1);
	seq_printf(s, "status_bar_%s_0x10: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status2);
	seq_printf(s, "matches_mz0380_fw_txt_01_11: %d\n",
		   status0 == 1 && status1 == 11);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_fw_status_command10);

static int hd60pro_windows_init_plan_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 fw_major;
	u32 fw_minor;
	u32 fw_marker;
	u32 pci_class_rev;

	pci_read_config_dword(hd->pdev, PCI_CLASS_REVISION, &pci_class_rev);
	fw_major = ioread32(hd60pro_mailbox_base(hd) + 0x008);
	fw_minor = ioread32(hd60pro_mailbox_base(hd) + 0x00c);
	fw_marker = ioread32(hd60pro_mailbox_base(hd) + 0x02c);

	seq_puts(s, "windows_init_plan_after_validated_0x01_0x0a\n");
	seq_printf(s, "pci_class_revision_raw: 0x%08x\n", pci_class_rev);
	seq_printf(s, "firmware_marker_bar0_0x2c: 0x%08x\n", fw_marker);
	seq_printf(s, "firmware_version_bar0_0x08_0x0c: %u.%02u\n",
		   fw_major, fw_minor);
	seq_printf(s, "firmware_matches_mz0380_fw_txt_01_11: %d\n",
		   fw_marker == 0xaaaaaaaa && fw_major == 1 && fw_minor == 11);
	seq_puts(s, "\n");
	seq_puts(s, "next_windows_helpers_identified:\n");
	seq_puts(s, "helper 0x140287c80: packet=[0x800,0x17,1<<line,(value&1)<<line], flags=0\n");
	seq_puts(s, "helper 0x140287d00: packet=[0x800,0x15,1<<line,(value&1)<<line], flags=0\n");
	seq_puts(s, "helper 0x1402777e4: packet=[0x800,0x1e,(len<<16)|(reg<<8)|selector], flags=0, returns BAR0+0x0c..0x28\n");
	seq_puts(s, "\n");
	seq_puts(s, "likely_generic_gpio_sequence_if_revision_low_nibble_not_2_or_3:\n");
	seq_puts(s, "[0x800,0x17,0x00000001,0x00000001]  line 0 = 1\n");
	seq_puts(s, "[0x800,0x17,0x00000002,0x00000000]  line 1 = 0\n");
	seq_puts(s, "[0x800,0x17,0x00000004,0x00000000]  line 2 = 0\n");
	seq_puts(s, "[0x800,0x17,0x00000040,0x00000040]  line 6 = 1\n");
	seq_puts(s, "[0x800,0x17,0x00000100,0x00000000]  line 8 = 0\n");
	seq_puts(s, "[0x800,0x17,0x00000200,0x00000000]  line 9 = 0\n");
	seq_puts(s, "[0x800,0x17,0x00000400,0x00000000]  line 10 = 0\n");
	seq_puts(s, "[0x800,0x17,0x00000800,0x00000000]  line 11 = 0\n");
	seq_puts(s, "\n");
	seq_puts(s, "blocked_by_design: these GPIO/I2C commands are not sent by this file\n");
	seq_puts(s, "reason: the exact Windows board-revision branch and side effects still need validation\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_init_plan);

static int hd60pro_gpio17_line0_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[4] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_GPIO_SET,
		BIT(0),
		BIT(0),
	};
	u32 completion = 0;
	u32 status0;
	u32 status1;
	u32 status2;
	int ret;

	seq_puts(s, "windows_gpio17_line0: first generic post-version GPIO command\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "planned_packet: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   packet[0], packet[1], packet[2], packet[3]);

	if (!allow_gpio_command17) {
		seq_puts(s, "blocked; reload with allow_gpio_command17=1 allow_mailbox_writes=1 after validated preinit/status to send this command\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x02c) != 0xaaaaaaaa ||
	    ioread32(base + 0x008) != 1 ||
	    ioread32(base + 0x00c) != 11) {
		seq_puts(s, "blocked; firmware status marker/version is not in the validated 0x0a state\n");
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	status0 = ioread32(base + 0x008);
	status1 = ioread32(base + 0x00c);
	status2 = ioread32(base + 0x010);
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "packet: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   packet[0], packet[1], packet[2], packet[3]);
	seq_printf(s, "result: %d\n", ret);
	seq_printf(s, "completion: 0x%08x\n", completion);
	seq_printf(s, "status_bar_%s_0x08: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status0);
	seq_printf(s, "status_bar_%s_0x0c: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status1);
	seq_printf(s, "status_bar_%s_0x10: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status2);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_gpio17_line0);

static int hd60pro_gpio17_generic_sequence_show(struct seq_file *s,
						void *unused)
{
	static const struct {
		u8 line;
		u8 value;
	} steps[] = {
		{ 0, 1 },
		{ 1, 0 },
		{ 2, 0 },
		{ 6, 1 },
		{ 8, 0 },
		{ 9, 0 },
		{ 10, 0 },
		{ 11, 0 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int ret = 0;

	seq_puts(s, "windows_gpio17_generic_sequence: post-version generic GPIO sequence\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_gpio17_sequence) {
		seq_puts(s, "blocked; reload with allow_gpio17_sequence=1 allow_mailbox_writes=1 after validated preinit/status to send this sequence\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x02c) != 0xaaaaaaaa ||
	    ioread32(base + 0x008) != 1 ||
	    ioread32(base + 0x00c) != 11) {
		seq_puts(s, "blocked; firmware status marker/version is not in the validated 0x0a state\n");
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		u32 mask = BIT(steps[i].line);
		u32 value = steps[i].value ? mask : 0;
		u32 packet[4] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_GPIO_SET,
			mask,
			value,
		};
		u32 completion = 0;

		ret = hd60pro_mailbox_send_locked(hd, packet,
						  ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s,
			   "step%u_line%u_value%u packet=0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   i, steps[i].line, steps[i].value,
			   packet[0], packet[1], packet[2], packet[3],
			   ret, completion, ioread32(base + 0x008),
			   ioread32(base + 0x00c), ioread32(base + 0x010));
		if (ret)
			break;
		if (hd60pro_mailbox_dead(base)) {
			ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", ret);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_gpio17_generic_sequence);

static int hd60pro_i2c1a_c0_dc_db_show(struct seq_file *s, void *unused)
{
	static const u8 regs[] = { 0xdc, 0xdb };
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 values[ARRAY_SIZE(regs)] = { 0 };
	unsigned int i;
	int ret = 0;

	seq_puts(s, "windows_i2c1a_c0_dc_db: first reads after generic GPIO sequence\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_i2c_read_command1a) {
		seq_puts(s, "blocked; reload with allow_i2c_read_command1a=1 allow_mailbox_writes=1 after validated preinit/status/gpio17 sequence to send these reads\n");
		seq_puts(s, "planned packets: [0x800,0x1a,0xc0,0xdc,0], [0x800,0x1a,0xc0,0xdb,0]\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_GPIO_SET ||
	    ioread32(base + 0x008) != BIT(11) ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like the completed generic GPIO17 sequence\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u32 packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_READ8,
			0xc0,
			regs[i],
			0,
		};
		u32 completion = 0;

		ret = hd60pro_mailbox_send_locked(hd, packet,
						  ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		values[i] = ioread32(base + 0x010);
		seq_printf(s,
			   "read%u_bus0xc0_reg0x%02x packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x value_bar0_010=0x%08x bar0_008=0x%08x bar0_00c=0x%08x\n",
			   i, regs[i], packet[0], packet[1], packet[2],
			   packet[3], packet[4], ret, completion, values[i],
			   ioread32(base + 0x008), ioread32(base + 0x00c));
		if (ret)
			break;
		if (hd60pro_mailbox_dead(base)) {
			ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "windows_condition_reg_dc_eq_0x05_and_reg_db_eq_0x32: %d\n",
		   (values[0] & 0xff) == 0x05 && (values[1] & 0xff) == 0x32);
	seq_printf(s, "result: %d\n", ret);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_i2c1a_c0_dc_db);

static int hd60pro_logo_upload_plan_show(struct seq_file *s, void *unused)
{
	static const struct {
		const char *name;
		const char *firmware;
		u32 selector;
		u64 windows_source_va;
	} logos[] = {
		{
			.name = "no_signal",
			.firmware = "hd60prodrv/logo-selector-100.bin",
			.selector = 0x100,
			.windows_source_va = 0x140319040ULL,
		},
		{
			.name = "hdcp",
			.firmware = "hd60prodrv/logo-selector-200.bin",
			.selector = 0x200,
			.windows_source_va = 0x1402f3840ULL,
		},
		{
			.name = "still",
			.firmware = "hd60prodrv/logo-selector-300.bin",
			.selector = 0x300,
			.windows_source_va = 0x140319040ULL,
		},
	};
	struct hd60pro_dev *hd = s->private;
	resource_size_t len = hd60pro_mailbox_len(hd);
	unsigned int i;

	seq_puts(s, "windows_logo_upload_plan_after_i2c_branch_false\n");
	seq_puts(s, "hardware_written: 0\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "copy_offset: 0x%x\n", HD60PRO_FW_WINDOW_OFFSET);
	seq_printf(s, "payload_size: 0x%x\n", HD60PRO_LOGO_PAYLOAD_SIZE);
	seq_printf(s, "required_window_bytes: 0x%x\n",
		   HD60PRO_FW_WINDOW_OFFSET + HD60PRO_LOGO_PAYLOAD_SIZE);
	seq_printf(s, "selected_aperture_len: %pa\n", &len);
	seq_printf(s, "aperture_fits_selected_mailbox_bar: %d\n",
		   HD60PRO_FW_WINDOW_OFFSET + HD60PRO_LOGO_PAYLOAD_SIZE <= len);
	seq_puts(s, "windows_prepare_packet_shape: [0x800,0x60,selector,0x25800,0x00f00140], flags=1\n");
	seq_puts(s, "windows_commit_packet_shape: [0x800,0x61,1], flags=1, then sleep 100ms and require BAR0+0x08 == 0\n");
	seq_puts(s, "note: this file is read-only; it only checks extracted payload availability\n");

	for (i = 0; i < ARRAY_SIZE(logos); i++) {
		const struct firmware *fw;
		int ret;

		ret = request_firmware(&fw, logos[i].firmware, &hd->pdev->dev);
		seq_printf(s,
			   "%s selector=0x%03x firmware=%s windows_source_va=0x%llx prepare=[0x%08x,0x%08x,0x%08x,0x%08x,0x%08x] commit=[0x%08x,0x%08x,0x%08x] available=%d",
			   logos[i].name, logos[i].selector, logos[i].firmware,
			   logos[i].windows_source_va, HD60PRO_MBOX_DOORBELL,
			   HD60PRO_MBOX_CMD_LOGO_PREPARE, logos[i].selector,
			   HD60PRO_LOGO_PAYLOAD_SIZE, HD60PRO_LOGO_DESCRIPTOR,
			   HD60PRO_MBOX_DOORBELL, HD60PRO_MBOX_CMD_LOGO_COMMIT,
			   1, ret == 0);
		if (ret) {
			seq_printf(s, " request_firmware_result=%d\n", ret);
			continue;
		}
		seq_printf(s, " size=%zu size_ok=%d first8=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
			   fw->size, fw->size == HD60PRO_LOGO_PAYLOAD_SIZE,
			   fw->size > 0 ? fw->data[0] : 0,
			   fw->size > 1 ? fw->data[1] : 0,
			   fw->size > 2 ? fw->data[2] : 0,
			   fw->size > 3 ? fw->data[3] : 0,
			   fw->size > 4 ? fw->data[4] : 0,
			   fw->size > 5 ? fw->data[5] : 0,
			   fw->size > 6 ? fw->data[6] : 0,
			   fw->size > 7 ? fw->data[7] : 0);
		release_firmware(fw);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_logo_upload_plan);

static int hd60pro_logo_upload_one(struct hd60pro_dev *hd, struct seq_file *s,
				   const char *label, const char *firmware,
				   u32 selector)
{
	const struct firmware *fw;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 prepare_packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_LOGO_PREPARE,
		selector,
		HD60PRO_LOGO_PAYLOAD_SIZE,
		HD60PRO_LOGO_DESCRIPTOR,
	};
	u32 commit_packet[3] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_LOGO_COMMIT,
		1,
	};
	u32 prepare_completion;
	u32 commit_completion = 0;
	u32 commit_irq_delta = 0;
	u32 prepare_window_start = 0;
	u32 prepare_window_end = 0;
	u32 status0;
	u32 status1;
	u32 status2;
	unsigned int success_wait_ms = 0;
	int ret;

	ret = request_firmware(&fw, firmware, &hd->pdev->dev);
	if (ret) {
		seq_printf(s, "%s request_firmware_result=%d firmware=%s\n",
			   label, ret, firmware);
		return ret;
	}

	if (fw->size != HD60PRO_LOGO_PAYLOAD_SIZE) {
		seq_printf(s, "%s payload_size_mismatch=%zu expected=%u firmware=%s\n",
			   label, fw->size, HD60PRO_LOGO_PAYLOAD_SIZE,
			   firmware);
		release_firmware(fw);
		return -EINVAL;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_logo_prepare_locked(hd, prepare_packet,
						       ARRAY_SIZE(prepare_packet),
						       HD60PRO_MBOX_ASYNC_TIMEOUT_MS,
						       &prepare_window_start,
						       &prepare_window_end);
	prepare_completion = ioread32(base + HD60PRO_REG_MBOX_COMPLETE);
	if (ret)
		goto out_unlock;

	memcpy_toio(base + HD60PRO_FW_WINDOW_OFFSET, fw->data, fw->size);
	wmb();

	ret = hd60pro_mailbox_send_async_locked(hd, commit_packet,
						ARRAY_SIZE(commit_packet),
						HD60PRO_VISIBLE_FW_COMMIT_TIMEOUT_MS,
						&commit_completion,
						&commit_irq_delta);
	for (success_wait_ms = 0; success_wait_ms < 100; success_wait_ms++) {
		status0 = ioread32(base + 0x008);
		if (status0 == 0 || status0 == U32_MAX)
			break;
		msleep(1);
	}

out_unlock:
	status0 = ioread32(base + 0x008);
	status1 = ioread32(base + 0x00c);
	status2 = ioread32(base + 0x010);
	mutex_unlock(&hd->mailbox_lock);
	release_firmware(fw);

	seq_printf(s, "%s selector=0x%03x firmware=%s prepare_result=%d prepare_completion=0x%08x window_start=0x%08x window_end=0x%08x commit_completion=0x%08x commit_irq_delta=%u post_commit_wait_ms=%u bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
		   label, selector, firmware, ret, prepare_completion,
		   prepare_window_start, prepare_window_end, commit_completion,
		   commit_irq_delta, success_wait_ms, status0, status1,
		   status2);

	return ret;
}

static int hd60pro_logo_upload_selector100_show(struct seq_file *s,
						void *unused)
{
	struct hd60pro_dev *hd = s->private;
	const struct firmware *fw;
	void __iomem *base = hd60pro_mailbox_base(hd);
	resource_size_t len = hd60pro_mailbox_len(hd);
	u32 prepare_packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_LOGO_PREPARE,
		0x100,
		HD60PRO_LOGO_PAYLOAD_SIZE,
		HD60PRO_LOGO_DESCRIPTOR,
	};
	u32 commit_packet[3] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_LOGO_COMMIT,
		1,
	};
	u32 prepare_completion = 0;
	u32 commit_completion = 0;
	u32 commit_irq_delta = 0;
	u32 prepare_window_start = 0;
	u32 prepare_window_end = 0;
	u32 status0;
	u32 status1;
	u32 status2;
	unsigned int success_wait_ms = 0;
	int ret;

	seq_puts(s, "windows_logo_upload_selector100_no_signal\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "firmware: hd60prodrv/logo-selector-100.bin\n");
	seq_printf(s, "prepare_packet: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   prepare_packet[0], prepare_packet[1], prepare_packet[2],
		   prepare_packet[3], prepare_packet[4]);
	seq_printf(s, "commit_packet: 0x%08x 0x%08x 0x%08x\n",
		   commit_packet[0], commit_packet[1], commit_packet[2]);

	if (!allow_logo_upload) {
		seq_puts(s, "blocked; reload with allow_logo_upload=1 allow_mailbox_writes=1 request_irq_vector=1 after validated preinit/status/gpio/i2c sequence\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (hd->irq < 0) {
		seq_puts(s, "disabled; request_irq_vector=1 is required for the async commit command 0x61\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (HD60PRO_FW_WINDOW_OFFSET + HD60PRO_LOGO_PAYLOAD_SIZE > len) {
		seq_puts(s, "blocked; selected mailbox BAR aperture is too small\n");
		seq_printf(s, "required_window_bytes: 0x%x\n",
			   HD60PRO_FW_WINDOW_OFFSET + HD60PRO_LOGO_PAYLOAD_SIZE);
		seq_printf(s, "selected_aperture_len: %pa\n", &len);
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_I2C_READ8 ||
	    ioread32(base + 0x00c) != 0xdb ||
	    ioread32(base + 0x010) != 0 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like the completed 0xc0:0xdb I2C read with zero result\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	ret = request_firmware(&fw, "hd60prodrv/logo-selector-100.bin",
			       &hd->pdev->dev);
	if (ret) {
		seq_printf(s, "request_firmware_result: %d\n", ret);
		return 0;
	}

	if (fw->size != HD60PRO_LOGO_PAYLOAD_SIZE) {
		seq_printf(s, "blocked; payload size mismatch: %zu\n", fw->size);
		release_firmware(fw);
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_logo_prepare_locked(hd, prepare_packet,
						       ARRAY_SIZE(prepare_packet),
						       HD60PRO_MBOX_ASYNC_TIMEOUT_MS,
						       &prepare_window_start,
						       &prepare_window_end);
	prepare_completion = ioread32(base + HD60PRO_REG_MBOX_COMPLETE);
	seq_printf(s, "prepare_result: %d\n", ret);
	seq_printf(s, "prepare_completion: 0x%08x\n", prepare_completion);
	seq_printf(s, "prepare_bar5_window_start_0x40: 0x%08x\n",
		   prepare_window_start);
	seq_printf(s, "prepare_bar5_window_end_0x48: 0x%08x\n",
		   prepare_window_end);
	if (ret)
		goto out_unlock;

	memcpy_toio(base + HD60PRO_FW_WINDOW_OFFSET, fw->data, fw->size);
	wmb();
	seq_printf(s, "copied_bytes: %zu\n", fw->size);

	ret = hd60pro_mailbox_send_async_locked(hd, commit_packet,
						ARRAY_SIZE(commit_packet),
						HD60PRO_VISIBLE_FW_COMMIT_TIMEOUT_MS,
						&commit_completion,
						&commit_irq_delta);
		seq_printf(s, "commit_result: %d\n", ret);
		seq_printf(s, "commit_completion: 0x%08x\n", commit_completion);
		seq_printf(s, "commit_irq_delta: %u\n", commit_irq_delta);
		for (success_wait_ms = 0; success_wait_ms < 100;
		     success_wait_ms++) {
			status0 = ioread32(base + 0x008);
			if (status0 == 0 || status0 == U32_MAX)
				break;
			msleep(1);
		}
		seq_printf(s, "post_commit_status0_wait_ms: %u\n",
			   success_wait_ms);

out_unlock:
	status0 = ioread32(base + 0x008);
	status1 = ioread32(base + 0x00c);
	status2 = ioread32(base + 0x010);
	mutex_unlock(&hd->mailbox_lock);
	release_firmware(fw);

	seq_printf(s, "status_bar_%s_0x08: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status0);
	seq_printf(s, "status_bar_%s_0x0c: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status1);
	seq_printf(s, "status_bar_%s_0x10: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status2);
	seq_printf(s, "windows_success_condition_bar_%s_0x08_eq_0: %d\n",
		   hd60pro_mailbox_bar_name(), status0 == 0);
	seq_printf(s, "result: %d\n", ret);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_logo_upload_selector100);

static int hd60pro_logo_upload_all_show(struct seq_file *s, void *unused)
{
	static const struct {
		const char *name;
		const char *firmware;
		u32 selector;
	} logos[] = {
		{ "no_signal", "hd60prodrv/logo-selector-100.bin", 0x100 },
		{ "hdcp", "hd60prodrv/logo-selector-200.bin", 0x200 },
		{ "still", "hd60prodrv/logo-selector-300.bin", 0x300 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	resource_size_t len = hd60pro_mailbox_len(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_logo_upload_all_three_status_images\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_puts(s, "note: Windows HwInitialize calls all three logo functions and does not branch on their return values\n");

	if (!allow_logo_upload) {
		seq_puts(s, "blocked; reload with allow_logo_upload=1 allow_mailbox_writes=1 request_irq_vector=1 after validated preinit/status/gpio/i2c sequence\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (hd->irq < 0) {
		seq_puts(s, "disabled; request_irq_vector=1 is required for async commands\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (HD60PRO_FW_WINDOW_OFFSET + HD60PRO_LOGO_PAYLOAD_SIZE > len) {
		seq_puts(s, "blocked; selected mailbox BAR aperture is too small\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_I2C_READ8 ||
	    ioread32(base + 0x00c) != 0xdb ||
	    ioread32(base + 0x010) != 0 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like the completed 0xc0:0xdb I2C read with zero result\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(logos); i++) {
		const struct firmware *fw;
		u32 prepare_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_LOGO_PREPARE,
			logos[i].selector,
			HD60PRO_LOGO_PAYLOAD_SIZE,
			HD60PRO_LOGO_DESCRIPTOR,
		};
		u32 commit_packet[3] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_LOGO_COMMIT,
			1,
		};
		u32 prepare_completion;
		u32 commit_completion = 0;
		u32 commit_irq_delta = 0;
		u32 prepare_window_start = 0;
		u32 prepare_window_end = 0;
		u32 status0;
		u32 status1;
		u32 status2;
		unsigned int success_wait_ms;
		int ret;

		seq_printf(s, "logo%u_%s selector=0x%03x firmware=%s\n",
			   i, logos[i].name, logos[i].selector,
			   logos[i].firmware);
		ret = request_firmware(&fw, logos[i].firmware, &hd->pdev->dev);
		if (ret) {
			seq_printf(s, "request_firmware_result: %d\n", ret);
			final_ret = ret;
			break;
		}
		if (fw->size != HD60PRO_LOGO_PAYLOAD_SIZE) {
			seq_printf(s, "payload_size_mismatch: %zu\n", fw->size);
			release_firmware(fw);
			final_ret = -EINVAL;
			break;
		}

		mutex_lock(&hd->mailbox_lock);
		ret = hd60pro_mailbox_send_logo_prepare_locked(hd,
							       prepare_packet,
							       ARRAY_SIZE(prepare_packet),
							       HD60PRO_MBOX_ASYNC_TIMEOUT_MS,
							       &prepare_window_start,
							       &prepare_window_end);
		prepare_completion = ioread32(base + HD60PRO_REG_MBOX_COMPLETE);
		seq_printf(s, "prepare_result: %d completion=0x%08x window_start=0x%08x window_end=0x%08x\n",
			   ret, prepare_completion, prepare_window_start,
			   prepare_window_end);
		if (ret) {
			mutex_unlock(&hd->mailbox_lock);
			release_firmware(fw);
			final_ret = ret;
			break;
		}

		memcpy_toio(base + HD60PRO_FW_WINDOW_OFFSET, fw->data, fw->size);
		wmb();
		seq_printf(s, "copied_bytes: %zu\n", fw->size);

		ret = hd60pro_mailbox_send_async_locked(hd, commit_packet,
							ARRAY_SIZE(commit_packet),
							HD60PRO_VISIBLE_FW_COMMIT_TIMEOUT_MS,
							&commit_completion,
							&commit_irq_delta);
		seq_printf(s, "commit_result: %d completion=0x%08x irq_delta=%u\n",
			   ret, commit_completion, commit_irq_delta);
		for (success_wait_ms = 0; success_wait_ms < 100;
		     success_wait_ms++) {
			status0 = ioread32(base + 0x008);
			if (status0 == 0 || status0 == U32_MAX)
				break;
			msleep(1);
		}
		status0 = ioread32(base + 0x008);
		status1 = ioread32(base + 0x00c);
		status2 = ioread32(base + 0x010);
		mutex_unlock(&hd->mailbox_lock);
		release_firmware(fw);

		seq_printf(s, "post_commit_wait_ms=%u status0=0x%08x status1=0x%08x status2=0x%08x success_eq0=%d\n",
			   success_wait_ms, status0, status1, status2,
			   status0 == 0);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_logo_upload_all);

static int hd60pro_pre_28548c_logo_uploads_local_show(struct seq_file *s,
						      void *unused)
{
	static const struct {
		const char *label;
		const char *firmware;
		u32 selector;
		const char *source;
	} uploads[] = {
		{
			.label = "call_140276624_selector100",
			.firmware = "hd60prodrv/logo-selector-100.bin",
			.selector = 0x100,
			.source = "copy source VA 0x140319040",
		},
		{
			.label = "call_140276a28_selector200",
			.firmware = "hd60prodrv/logo-selector-200.bin",
			.selector = 0x200,
			.source = "copy source VA 0x1402f3840",
		},
		{
			.label = "call_140276dbc_selector300",
			.firmware = "hd60prodrv/logo-selector-300.bin",
			.selector = 0x300,
			.source = "copy source VA 0x140319040",
		},
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	resource_size_t len = hd60pro_mailbox_len(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_pre_28548c_logo_uploads_local\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS calls 0x140276624, 0x140276a28, 0x140276dbc immediately before 0x14028548c in the A3 setup path\n");
	seq_puts(s, "local branch: PCI/context bytes 0x0e/0x0f are treated as zero; helper conversion inputs are logged but the actual device copy uses the three extracted 0x25800-byte payloads\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_logo_upload) {
		seq_puts(s, "blocked; reload with allow_logo_upload=1 allow_mailbox_writes=1 request_irq_vector=1\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (hd->irq < 0) {
		seq_puts(s, "disabled; request_irq_vector=1 is required for async commands\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (HD60PRO_FW_WINDOW_OFFSET + HD60PRO_LOGO_PAYLOAD_SIZE > len) {
		seq_puts(s, "blocked; selected mailbox BAR aperture is too small\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: post-logo pipeline has already started\n");
		return 0;
	}

	if (!((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_I2C_READ8 &&
	       ioread32(base + 0x00c) == 0xdb &&
	       ioread32(base + 0x010) == 0 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_LOGO_COMMIT &&
	       ioread32(base + 0x008) == 1 &&
	       ioread32(base + 0x00c) == HD60PRO_LOGO_PAYLOAD_SIZE &&
	       ioread32(base + 0x010) == HD60PRO_LOGO_DESCRIPTOR &&
	       ioread32(base + 0x02c) == 1))) {
		seq_puts(s, "blocked; last mailbox state is neither completed I2C zero read nor completed logo commit\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(uploads); i++) {
		int ret;

		seq_printf(s, "%s: %s\n", uploads[i].label,
			   uploads[i].source);
		ret = hd60pro_logo_upload_one(hd, s, uploads[i].label,
					      uploads[i].firmware,
					      uploads[i].selector);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_pre_28548c_logo_uploads_local);

static int hd60pro_post_logo_plan_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);

	seq_puts(s, "windows_post_logo_plan\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14027a498..0x14027a52f\n");
	seq_puts(s, "after logo uploads Windows calls:\n");
	seq_puts(s, "  0x14028548c: video/pipeline mode config using context 0x73a8/0x73b8/0x73bc/0x73c0\n");
	seq_puts(s, "    local 1cfa:0006/default path decoded so far: helper 0x14028658c sends [0x800,0x1b,0x9c,0x00,0x02] if bank changed, then [0x800,0x1b,0x9c,0x18,0x00]\n");
	seq_puts(s, "  0x140286734: long sensor/decoder config path, called with edx=0 r8d=1\n");
	seq_puts(s, "  0x140287224: long color/scaler config path, called with edx=1 r8d=1\n");
	seq_puts(s, "  0x140287b54: packet [0x800,0x1d,0xa2,0x11,<low byte of context+0x1d0b0>,1]\n");
	seq_puts(s, "  0x140287b54: packet [0x800,0x1d,0xa2,0x12,0x5a,1]\n");
	seq_puts(s, "  0x140287b54: packet [0x800,0x1d,0xa2,0x10,0x5a,1]\n");
	seq_puts(s, "context+0x1d0b0 default: init path loads a registry/config value with default 0; setter stores value|0x80000000, but this call uses only the low byte\n");
	seq_puts(s, "linux test assumption for first 0x1d write: value 0x00\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	if (base) {
		seq_printf(s, "current_bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "current_bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "current_bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "current_bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "current_bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_plan);

static int hd60pro_post_logo_pipeline_28548c_min_show(struct seq_file *s,
						      void *unused)
{
	static const struct {
		const char *name;
		u32 reg;
		u32 value;
	} writes[] = {
		{ "select_bank_2_if_needed", 0x00, 0x02 },
		{ "local_mode_edx0_value0", 0x18, 0x00 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_28548c_min\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x140285435..0x140285461 via helper 0x14028658c\n");
	seq_puts(s, "local assumptions: context+0x73a8=0, function edx=0, PCI config bytes 0x0e/0x0f do not take alternate branch\n");
	seq_puts(s, "planned packets: [0x800,0x1b,0x9c,0x00,0x02], [0x800,0x1b,0x9c,0x18,0x00]\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated logo_upload_all\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_LOGO_COMMIT ||
	    ioread32(base + 0x008) != 1 ||
	    ioread32(base + 0x00c) != HD60PRO_LOGO_PAYLOAD_SIZE ||
	    ioread32(base + 0x010) != HD60PRO_LOGO_DESCRIPTOR ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed logo_upload_all commit\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		u32 packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			writes[i].reg,
			writes[i].value,
		};
		u32 completion = 0;
		u32 status0;
		u32 status1;
		u32 status2;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		status0 = ioread32(base + 0x008);
		status1 = ioread32(base + 0x00c);
		status2 = ioread32(base + 0x010);
		seq_printf(s, "write%u_%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   i, writes[i].name, packet[0], packet[1], packet[2],
			   packet[3], packet[4], ret, completion, status0,
			   status1, status2);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_28548c_min);

static bool hd60pro_24dc28_state_at_or_after(void __iomem *base,
					     unsigned int stage);

static int hd60pro_post_logo_pipeline_24dc28_head_local_show(struct seq_file *s,
							     void *unused)
{
	static const struct {
		const char *name;
		u32 packet[5];
		u8 words;
		u16 sleep_ms;
	} steps[] = {
		{
			.name = "gpio15_alt_line9_value1",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_GPIO_ALT_SET,
				BIT(9),
				BIT(9),
				0,
			},
			.words = 4,
			.sleep_ms = 50,
		},
		{
			.name = "gpio15_alt_line9_value0",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_GPIO_ALT_SET,
				BIT(9),
				0,
				0,
			},
			.words = 4,
			.sleep_ms = 50,
		},
		{
			.name = "gpio15_alt_line9_value1_again",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_GPIO_ALT_SET,
				BIT(9),
				BIT(9),
				0,
			},
			.words = 4,
			.sleep_ms = 50,
		},
		{
			.name = "gpio15_alt_line8_value0",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_GPIO_ALT_SET,
				BIT(8),
				0,
				0,
			},
			.words = 4,
			.sleep_ms = 50,
		},
		{
			.name = "select_bank0_for_9c",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0x00,
				0x00,
			},
			.words = 5,
			.sleep_ms = 0,
		},
		{
			.name = "write_9c_13_head_value8",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0x13,
				0x08,
			},
			.words = 5,
			.sleep_ms = 0,
		},
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_head_local\n");
	seq_puts(s, "source: beginning of e60MZ0380.X64.SYS 0x14024dc28, called by true local 0x14028548c before 88:03=a7\n");
	seq_puts(s, "local assumptions: PCI config byte 0x0e is not 0x55/0xc5, so the fourth GPIO-alt command is line8=0\n");
	seq_puts(s, "planned packets: GPIO15-alt line9 1/0/1 with 50 ms sleeps, line8=0, then 9c:00=0 and 9c:13=8\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_28548c_min\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 1)) {
		seq_puts(s, "already_completed: head or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0x13 &&
	    ioread32(base + 0x010) == 0x08 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final head state bank0 9c:13=8 observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x18 ||
	    ioread32(base + 0x010) != 0x00 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_28548c_min\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, steps[i].packet,
						  steps[i].words,
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "%s packet=", steps[i].name);
		if (steps[i].words == 4)
			seq_printf(s, "0x%08x,0x%08x,0x%08x,0x%08x",
				   steps[i].packet[0], steps[i].packet[1],
				   steps[i].packet[2], steps[i].packet[3]);
		else
			seq_printf(s, "0x%08x,0x%08x,0x%08x,0x%08x,0x%08x",
				   steps[i].packet[0], steps[i].packet[1],
				   steps[i].packet[2], steps[i].packet[3],
				   steps[i].packet[4]);
		seq_printf(s, " result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   ret, completion, ioread32(base + 0x008),
			   ioread32(base + 0x00c), ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
		if (steps[i].sleep_ms)
			msleep(steps[i].sleep_ms);
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_head_local);

static bool hd60pro_24dc28_state_at_or_after(void __iomem *base,
					     unsigned int stage)
{
	u32 cmd = ioread32(base + 0x004);
	u32 status = ioread32(base + 0x008);
	u32 reg = ioread32(base + 0x00c);
	u32 value = ioread32(base + 0x010);
	u32 completion = ioread32(base + 0x02c);

	if (cmd != HD60PRO_MBOX_CMD_PIPELINE_WRITE || completion != 1)
		return false;

	if (status == 0x44 && stage <= 7 && reg == 0x03)
		return true;

	if (status != 0x4e)
		return false;

	if (stage <= 1 && reg == 0x19 && value == 0x02)
		return true;
	if (stage <= 2 && reg == 0x08 && value == 0x03)
		return true;
	if (stage <= 3 && reg == 0xb4)
		return true;
	if (stage <= 4 && reg == 0x2e && value == 0xa1)
		return true;
	if (stage <= 5 && reg == 0xb7 && value == 0x00)
		return true;
	if (stage <= 6 && reg == 0x00)
		return true;
	if (stage <= 7 && reg == 0xcf)
		return true;
	if (stage <= 7 && reg == 0x1f)
		return true;
	if (stage <= 7 && reg == 0xa9)
		return true;

	return false;
}

static int hd60pro_post_logo_pipeline_24dc28_table1_local_show(struct seq_file *s,
							       void *unused)
{
	static const struct {
		const char *name;
		u32 reg;
		u32 value;
	} writes[] = {
		{ "write_9c_41_6f", 0x41, 0x6f },
		{ "write_9c_b8_00", 0xb8, 0x00 },
		{ "select_bank1_for_9c", 0x00, 0x01 },
		{ "write_9c_0f_bank1_02", 0x0f, 0x02 },
		{ "write_9c_16_bank1_30", 0x16, 0x30 },
		{ "select_bank0_for_9c", 0x00, 0x00 },
		{ "write_9c_64_02", 0x64, 0x02 },
		{ "write_9c_65_ff", 0x65, 0xff },
		{ "write_9c_66_00", 0x66, 0x00 },
		{ "write_9c_67_02", 0x67, 0x02 },
		{ "select_bank1_for_local_17_18_19", 0x00, 0x01 },
		{ "write_9c_17_local_02", 0x17, 0x02 },
		{ "write_9c_18_local_02", 0x18, 0x02 },
		{ "write_9c_19_local_02", 0x19, 0x02 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table1_local\n");
	seq_puts(s, "source: first deterministic table slice of e60MZ0380.X64.SYS 0x14024dc28 after head 9c:13=8\n");
	seq_puts(s, "local assumptions: context+0x81d8=0 path writes bank1 9c:17/18/19=2; read/modify blocks after 9c:1a are not sent here\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_head_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 1)) {
		seq_puts(s, "already_completed: table1 or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x13 ||
	    ioread32(base + 0x010) != 0x08 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_head_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		u32 packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			writes[i].reg,
			writes[i].value,
		};
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   writes[i].name, packet[0], packet[1], packet[2],
			   packet[3], packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table1_local);

static int hd60pro_post_logo_pipeline_24dc28_table2_local_show(struct seq_file *s,
							       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 read2a = 0;
	u32 value2a;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table2_local\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024df64..0x14024dfc9, continuation after local/default 9c:19=2\n");
	seq_puts(s, "planned packets: bank1 9c:1a=0x50, read bank1 9c:2a, write 9c:2a|7, then bank2 9c:08=3\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table1_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 2)) {
		seq_puts(s, "already_completed: table2 or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x19 ||
	    ioread32(base + 0x010) != 0x02 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table1_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	{
		u32 completion = 0;
		u32 bank1_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			0x00,
			0x01,
		};
		u32 write1a_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			0x1a,
			0x50,
		};
		u32 read2a_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_READ8,
			0x9c,
			0x2a,
			0,
		};
		u32 bank2_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			0x00,
			0x02,
		};
		u32 write08_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			0x08,
			0x03,
		};
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, bank1_packet,
						  ARRAY_SIZE(bank1_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "select_bank1 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   bank1_packet[0], bank1_packet[1], bank1_packet[2],
			   bank1_packet[3], bank1_packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		ret = hd60pro_mailbox_send_locked(hd, write1a_packet,
						  ARRAY_SIZE(write1a_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "write_9c_1a_bank1_50 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   write1a_packet[0], write1a_packet[1],
			   write1a_packet[2], write1a_packet[3],
			   write1a_packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		ret = hd60pro_mailbox_send_locked(hd, read2a_packet,
						  ARRAY_SIZE(read2a_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		read2a = ioread32(base + 0x010);
		value2a = (read2a | 0x07) & 0xff;
		seq_printf(s, "read_9c_2a_bank1 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x value=0x%08x value_or_07=0x%02x bar0_008=0x%08x bar0_00c=0x%08x\n",
			   read2a_packet[0], read2a_packet[1],
			   read2a_packet[2], read2a_packet[3],
			   read2a_packet[4], ret, completion, read2a,
			   value2a, ioread32(base + 0x008),
			   ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		{
			u32 write2a_packet[5] = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0x2a,
				value2a,
			};

			ret = hd60pro_mailbox_send_locked(hd, write2a_packet,
							  ARRAY_SIZE(write2a_packet),
							  HD60PRO_MBOX_TIMEOUT_US,
							  &completion);
			seq_printf(s, "write_9c_2a_bank1_or07 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
				   write2a_packet[0], write2a_packet[1],
				   write2a_packet[2], write2a_packet[3],
				   write2a_packet[4], ret, completion,
				   ioread32(base + 0x008),
				   ioread32(base + 0x00c),
				   ioread32(base + 0x010));
			if (ret) {
				final_ret = ret;
				goto out_unlock;
			}
		}

		ret = hd60pro_mailbox_send_locked(hd, bank2_packet,
						  ARRAY_SIZE(bank2_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "select_bank2 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   bank2_packet[0], bank2_packet[1], bank2_packet[2],
			   bank2_packet[3], bank2_packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		ret = hd60pro_mailbox_send_locked(hd, write08_packet,
						  ARRAY_SIZE(write08_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "write_9c_08_bank2_03 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   write08_packet[0], write08_packet[1],
			   write08_packet[2], write08_packet[3],
			   write08_packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret)
			final_ret = ret;
	}

out_unlock:
	if (!final_ret && hd60pro_mailbox_dead(base)) {
		final_ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table2_local);

static int hd60pro_pipeline_write8_locked(struct hd60pro_dev *hd,
					  struct seq_file *s,
					  const char *name, u32 reg,
					  u32 value)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PIPELINE_WRITE,
		0x9c,
		reg,
		value & 0xff,
	};
	u32 completion = 0;
	int ret;

	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	seq_printf(s, "%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
		   name, packet[0], packet[1], packet[2], packet[3],
		   packet[4], ret, completion, ioread32(base + 0x008),
		   ioread32(base + 0x00c), ioread32(base + 0x010));

	return ret;
}

static int hd60pro_i2c_read8_locked(struct hd60pro_dev *hd,
				    struct seq_file *s, const char *name,
				    u32 chip, u32 reg, u32 *value)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_I2C_READ8,
		chip,
		reg,
		0,
	};
	u32 completion = 0;
	int ret;

	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	*value = ioread32(base + 0x010);
	seq_printf(s, "%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x value=0x%08x bar0_008=0x%08x bar0_00c=0x%08x\n",
		   name, packet[0], packet[1], packet[2], packet[3],
		   packet[4], ret, completion, *value,
		   ioread32(base + 0x008), ioread32(base + 0x00c));

	return ret;
}

static int hd60pro_i2c_write8_locked(struct hd60pro_dev *hd,
				     struct seq_file *s, const char *name,
				     u32 chip, u32 reg, u32 value)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PIPELINE_WRITE,
		chip,
		reg,
		value & 0xff,
	};
	u32 completion = 0;
	int ret;

	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	seq_printf(s, "%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
		   name, packet[0], packet[1], packet[2], packet[3],
		   packet[4], ret, completion, ioread32(base + 0x008),
		   ioread32(base + 0x00c), ioread32(base + 0x010));

	return ret;
}

static int hd60pro_pipeline_read8_locked(struct hd60pro_dev *hd,
					 struct seq_file *s,
					 const char *name, u32 reg,
					 u32 *value)
{
	return hd60pro_i2c_read8_locked(hd, s, name, 0x9c, reg, value);
}

static int hd60pro_selector_read8_locked(struct hd60pro_dev *hd,
					 struct seq_file *s,
					 const char *name, u32 selector,
					 u32 reg, u32 *value)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packed = (1u << 16) | ((reg & 0xff) << 8) | (selector & 0xff);
	u32 packet[3] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_SELECTOR_READ,
		packed,
	};
	u32 completion = 0;
	int ret;

	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	*value = ioread32(base + 0x00c);
	seq_printf(s, "%s selector=0x%02x reg=0x%02x packed=0x%08x result=%d completion=0x%08x value=0x%02x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
		   name, selector, reg, packed, ret, completion, *value & 0xff,
		   ioread32(base + 0x008), ioread32(base + 0x00c),
		   ioread32(base + 0x010));

	return ret;
}

static int __maybe_unused hd60pro_selector_write8_locked(struct hd60pro_dev *hd,
							 struct seq_file *s,
							 const char *name,
							 u32 selector, u32 reg,
							 u32 value)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PIPELINE_WRITE,
		selector,
		reg,
		value & 0xff,
	};
	u32 completion = 0;
	int ret;

	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	seq_printf(s, "%s selector=0x%02x reg=0x%02x value=0x%02x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
		   name, selector, reg, value & 0xff, ret, completion,
		   ioread32(base + 0x008), ioread32(base + 0x00c),
		   ioread32(base + 0x010));

	return ret;
}

static int hd60pro_post_logo_pipeline_24dc28_table3_local_show(struct seq_file *s,
							       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 read24 = 0;
	u32 readae = 0;
	u32 readb4 = 0;
	u32 valueae = 0;
	u32 valuead = 0x05;
	u32 valueb4 = 0;
	unsigned int i;
	int final_ret = 0;
	int ret;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table3_local\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024e085..0x14024e282 local/default path after bank2 9c:08=3\n");
	seq_puts(s, "local assumptions: PCI config bytes 0x0e/0x0f are 0, context+0x97bc=0 => bank1 9c:24=0x40, context+0x81f8=0 => bank0 9c:ad=5\n");
	seq_puts(s, "planned packets: bank1 24/read24/optional 25-27, 30/31/32; bank0 b0, ae|4, ad, b1/b2/b3, b4 then b4&0xfc\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table2_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 3)) {
		seq_puts(s, "already_completed: table3 or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x08 ||
	    ioread32(base + 0x010) != 0x03 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table2_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);

	ret = hd60pro_pipeline_write8_locked(hd, s, "select_bank1_for_24_30_32",
					     0x00, 0x01);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_24_bank1_40",
					     0x24, 0x40);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_24_bank1",
					    0x24, &read24);
	if (ret)
		goto fail;

	if (read24 & 0x01) {
		ret = hd60pro_pipeline_write8_locked(hd, s,
						     "write_9c_25_bank1_00",
						     0x25, 0x00);
		if (ret)
			goto fail;
		ret = hd60pro_pipeline_write8_locked(hd, s,
						     "write_9c_26_bank1_00",
						     0x26, 0x00);
		if (ret)
			goto fail;
		for (i = 0; i < 5; i++) {
			ret = hd60pro_pipeline_write8_locked(hd, s,
							     "write_9c_27_bank1_00_loop",
							     0x27, 0x00);
			if (ret)
				goto fail;
		}
	} else {
		seq_puts(s, "skip_bank1_25_26_27_loop: read_9c_24 bit0 is clear\n");
	}

	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_30_bank1_80",
					     0x30, 0x80);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_31_bank1_00",
					     0x31, 0x00);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_32_bank1_00",
					     0x32, 0x00);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_write8_locked(hd, s, "select_bank0_for_b0_b4",
					     0x00, 0x00);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_b0_bank0_14",
					     0xb0, 0x14);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_ae_bank0",
					    0xae, &readae);
	if (ret)
		goto fail;
	valueae = (readae | 0x04) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_ae_bank0_or04",
					     0xae, valueae);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_ad_bank0_default05",
					     0xad, valuead);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_b1_bank0_c0",
					     0xb1, 0xc0);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_b2_bank0_00",
					     0xb2, 0x00);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_b3_bank0_local00",
					     0xb3, 0x00);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_b4_bank0_55",
					     0xb4, 0x55);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_b4_bank0",
					    0xb4, &readb4);
	if (ret)
		goto fail;
	valueb4 = readb4 & 0xfc;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_b4_bank0_andfc",
					     0xb4, valueb4);
	if (ret)
		goto fail;

	goto out_unlock;

fail:
	final_ret = ret;

out_unlock:
	if (!final_ret && hd60pro_mailbox_dead(base)) {
		final_ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "read_9c_24: 0x%08x\n", read24);
	seq_printf(s, "read_9c_ae: 0x%08x write_ae: 0x%02x\n",
		   readae, valueae & 0xff);
	seq_printf(s, "read_9c_b4: 0x%08x write_b4: 0x%02x\n",
		   readb4, valueb4 & 0xff);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table3_local);

static int hd60pro_post_logo_pipeline_24dc28_table4_local_show(struct seq_file *s,
							       void *unused)
{
	static const struct {
		const char *name;
		u32 reg;
		u32 value;
	} writes[] = {
		{ "select_bank2_for_post_b4", 0x00, 0x02 },
		{ "write_9c_01_bank2_61", 0x01, 0x61 },
		{ "write_9c_02_bank2_f5", 0x02, 0xf5 },
		{ "write_9c_04_bank2_01", 0x04, 0x01 },
		{ "write_9c_05_bank2_00", 0x05, 0x00 },
		{ "write_9c_06_bank2_08", 0x06, 0x08 },
		{ "write_9c_1c_bank2_1a", 0x1c, 0x1a },
		{ "write_9c_1d_bank2_00", 0x1d, 0x00 },
		{ "write_9c_1e_bank2_00", 0x1e, 0x00 },
		{ "write_9c_1f_bank2_00", 0x1f, 0x00 },
		{ "write_9c_17_bank2_c0", 0x17, 0xc0 },
		{ "write_9c_19_bank2_ff", 0x19, 0xff },
		{ "write_9c_1a_bank2_ff", 0x1a, 0xff },
		{ "write_9c_1b_bank2_fc", 0x1b, 0xfc },
		{ "write_9c_20_bank2_00", 0x20, 0x00 },
		{ "write_9c_22_bank2_26", 0x22, 0x26 },
		{ "write_9c_27_bank2_local00", 0x27, 0x00 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 read03 = 0;
	u32 read25 = 0;
	u32 read02 = 0;
	u32 read07 = 0;
	u32 read21 = 0;
	u32 read2e = 0;
	u32 value03 = 0;
	u32 value25 = 0;
	u32 value02 = 0;
	u32 value07 = 0;
	u32 value21 = 0;
	u32 value2e = 0;
	unsigned int i;
	int final_ret = 0;
	int ret;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table4_local\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024e287..0x14024e54f, bank2 continuation after table3 b4\n");
	seq_puts(s, "local assumptions: context+0x73bc=0 => bank2 9c:27=0; stop before repeated bank0 ab/ac and helper calls\n");
	seq_puts(s, "planned packets: bank2 01/02, RMW 03, writes 04/05/06/1c/1d/1e/1f, RMW 25/02/07, writes 17/19/1a/1b/20, RMW 21, write 22/27, RMW 2e\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table3_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 4)) {
		seq_puts(s, "already_completed: table4 or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0x2e &&
	    ioread32(base + 0x010) == 0xa1 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final table4 state bank2 9c:2e=0xa1 observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0xb4 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table3_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);

	for (i = 0; i < 3; i++) {
		ret = hd60pro_pipeline_write8_locked(hd, s, writes[i].name,
						     writes[i].reg,
						     writes[i].value);
		if (ret)
			goto fail;
	}

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_03_bank2",
					    0x03, &read03);
	if (ret)
		goto fail;
	value03 = (read03 | 0x02) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_03_bank2_or02",
					     0x03, value03);
	if (ret)
		goto fail;

	for (i = 3; i < 10; i++) {
		ret = hd60pro_pipeline_write8_locked(hd, s, writes[i].name,
						     writes[i].reg,
						     writes[i].value);
		if (ret)
			goto fail;
	}

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_25_bank2",
					    0x25, &read25);
	if (ret)
		goto fail;
	value25 = (read25 | 0xa2) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_25_bank2_ora2",
					     0x25, value25);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_02_bank2",
					    0x02, &read02);
	if (ret)
		goto fail;
	value02 = (read02 | 0x80) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_02_bank2_or80",
					     0x02, value02);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_07_bank2",
					    0x07, &read07);
	if (ret)
		goto fail;
	value07 = (read07 | 0x04) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_07_bank2_or04",
					     0x07, value07);
	if (ret)
		goto fail;

	for (i = 10; i < ARRAY_SIZE(writes); i++) {
		ret = hd60pro_pipeline_write8_locked(hd, s, writes[i].name,
						     writes[i].reg,
						     writes[i].value);
		if (ret)
			goto fail;
		if (writes[i].reg == 0x20)
			break;
	}

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_21_bank2",
					    0x21, &read21);
	if (ret)
		goto fail;
	value21 = read21 & 0xfc;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_21_bank2_andfc",
					     0x21, value21);
	if (ret)
		goto fail;

	for (i = 15; i < ARRAY_SIZE(writes); i++) {
		ret = hd60pro_pipeline_write8_locked(hd, s, writes[i].name,
						     writes[i].reg,
						     writes[i].value);
		if (ret)
			goto fail;
	}

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_2e_bank2",
					    0x2e, &read2e);
	if (ret)
		goto fail;
	value2e = (read2e | 0xa1) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_2e_bank2_ora1",
					     0x2e, value2e);
	if (ret)
		goto fail;

	goto out_unlock;

fail:
	final_ret = ret;

out_unlock:
	if (!final_ret && hd60pro_mailbox_dead(base)) {
		final_ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "read03=0x%08x write03=0x%02x read25=0x%08x write25=0x%02x read02=0x%08x write02=0x%02x\n",
		   read03, value03 & 0xff, read25, value25 & 0xff,
		   read02, value02 & 0xff);
	seq_printf(s, "read07=0x%08x write07=0x%02x read21=0x%08x write21=0x%02x read2e=0x%08x write2e=0x%02x\n",
		   read07, value07 & 0xff, read21, value21 & 0xff,
		   read2e, value2e & 0xff);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table4_local);

static int hd60pro_post_logo_helper_24eeb8_locked(struct hd60pro_dev *hd,
						  struct seq_file *s,
						  const char *name,
						  u32 windows_edx)
{
	u32 readb7 = 0;
	u32 valueb7;
	int ret;

	seq_printf(s, "%s: source 0x14024eeb8 local/default edx=0x%02x\n",
		   name, windows_edx & 0xff);

	/*
	 * With the current local assumptions, the Windows helper reaches the
	 * fallback GPIO-alt path and then updates bank0 9c:b7 from its input
	 * bitmask. edx bit1 keeps b7 bit1 set; edx bit5 clears it.
	 */
	ret = hd60pro_mailbox_send_locked(hd,
					  (u32 []) {
						  HD60PRO_MBOX_DOORBELL,
						  HD60PRO_MBOX_CMD_GPIO_ALT_SET,
						  BIT(1),
						  0,
					  },
					  4, HD60PRO_MBOX_TIMEOUT_US, &(u32){ 0 });
	if (ret)
		return ret;
	seq_printf(s, "%s_gpio15_alt_line1_value0 result=%d\n", name, ret);

	ret = hd60pro_pipeline_read8_locked(hd, s, "helper_read_9c_b7_bank0",
					    0xb7, &readb7);
	if (ret)
		return ret;

	valueb7 = (readb7 | 0x02) & 0xff;
	if (windows_edx & 0x20)
		valueb7 &= ~0x02;

	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "helper_write_9c_b7_bank0",
					     0xb7, valueb7);
	seq_printf(s, "%s_read_b7=0x%08x %s_write_b7=0x%02x\n",
		   name, readb7, name, valueb7 & 0xff);

	return ret;
}

static int hd60pro_post_logo_helper_24d2a4_locked(struct hd60pro_dev *hd,
						  struct seq_file *s)
{
	int ret;

	seq_puts(s, "helper_24d2a4: write bank0 9c:b8=0x10 then 0x00\n");
	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "helper_24d2a4_write_b8_10",
					     0xb8, 0x10);
	if (ret)
		return ret;

	return hd60pro_pipeline_write8_locked(hd, s,
					      "helper_24d2a4_write_b8_00",
					      0xb8, 0x00);
}

static int hd60pro_post_logo_helper_24db30_locked(struct hd60pro_dev *hd,
						  struct seq_file *s)
{
	int ret;

	seq_puts(s, "helper_24db30: write bank2 9c:07=0xf4 then 0x04\n");
	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "helper_24db30_select_bank2",
					     0x00, 0x02);
	if (ret)
		return ret;
	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "helper_24db30_write_07_f4",
					     0x07, 0xf4);
	if (ret)
		return ret;

	return hd60pro_pipeline_write8_locked(hd, s,
					      "helper_24db30_write_07_04",
					      0x07, 0x04);
}

static int hd60pro_post_logo_pipeline_24dc28_table5_local_show(struct seq_file *s,
							       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 readab = 0;
	u32 readac = 0;
	u32 valueab = 0;
	u32 valueac = 0;
	int final_ret = 0;
	int ret;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table5_local\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024e554..0x14024e84a local/default continuation after table4\n");
	seq_puts(s, "local assumptions: context+0x73a8=0, PCI config bytes 0x0e/0x0f are 0, path writes 9c:51=0x89 and calls helper_24eeb8(0x30)\n");
	seq_puts(s, "planned: bank0 ab/ac RMW, helper_24eeb8(0), helper_24d2a4, helper_24db30, 9c:51=0x89, helper_24eeb8(0x30), 9c:b7=0; stop before 0x14024d2ec\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table4_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 5)) {
		seq_puts(s, "already_completed: table5 or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0xb7 &&
	    ioread32(base + 0x010) == 0x00 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final table5 state bank0 9c:b7=0 observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x2e ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table4_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);

	ret = hd60pro_pipeline_write8_locked(hd, s, "select_bank0_for_ab_ac",
					     0x00, 0x00);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_ab_bank0",
					    0xab, &readab);
	if (ret)
		goto fail;
	valueab = (readab & 0x95) | 0x15;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_ab_bank0_rmw",
					     0xab, valueab);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_ac_bank0",
					    0xac, &readac);
	if (ret)
		goto fail;
	valueac = (readac & 0xd5) | 0x15;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_ac_bank0_rmw",
					     0xac, valueac);
	if (ret)
		goto fail;

	ret = hd60pro_post_logo_helper_24eeb8_locked(hd, s,
						     "helper_24eeb8_first",
						     0x00);
	if (ret)
		goto fail;
	ret = hd60pro_post_logo_helper_24d2a4_locked(hd, s);
	if (ret)
		goto fail;
	ret = hd60pro_post_logo_helper_24db30_locked(hd, s);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "select_bank0_after_helper_24db30",
					     0x00, 0x00);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "local_branch_write_9c_51_89",
					     0x51, 0x89);
	if (ret)
		goto fail;
	ret = hd60pro_post_logo_helper_24eeb8_locked(hd, s,
						     "helper_24eeb8_second",
						     0x30);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_write8_locked(hd, s,
					     "write_9c_b7_bank0_00_before_24d2ec",
					     0xb7, 0x00);
	if (ret)
		goto fail;

	goto out_unlock;

fail:
	final_ret = ret;

out_unlock:
	if (!final_ret && hd60pro_mailbox_dead(base)) {
		final_ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "read_ab=0x%08x write_ab=0x%02x read_ac=0x%08x write_ac=0x%02x\n",
		   readab, valueab & 0xff, readac, valueac & 0xff);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table5_local);

static int hd60pro_post_logo_pipeline_24dc28_table6_local_show(struct seq_file *s,
							       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 read01 = 0;
	u32 read04 = 0;
	u32 read09 = 0;
	u32 read54 = 0;
	u32 readac = 0;
	u32 read00_set = 0;
	u32 readce = 0;
	u32 readcf = 0;
	u32 read00_clear = 0;
	u32 value01 = 0;
	u32 value04 = 0;
	u32 value09 = 0;
	u32 value54 = 0;
	u32 valueac = 0;
	u32 value00_set = 0;
	u32 valuece = 0;
	u32 valuecf = 0;
	u32 value00_clear = 0;
	int final_ret = 0;
	int ret;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table6_local\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024d2ec head, called after table5 by 0x14024dc28\n");
	seq_puts(s, "planned: bank2 01|60, 04|1, 06=8, 09|20; bank0 54&ef, ac|80, 00|80, ce|80, cf=(read&fa)|2, 00&7f\n");
	seq_puts(s, "stop before context-dependent 9c:d0/9c:cf dynamic block\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table5_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 6)) {
		seq_puts(s, "already_completed: table6 or later 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0x00 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final table6 state bank0 9c:00 updated observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0xb7 ||
	    ioread32(base + 0x010) != 0x00 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table5_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);

	ret = hd60pro_pipeline_write8_locked(hd, s, "select_bank2_for_24d2ec_head",
					     0x00, 0x02);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_01_bank2",
					    0x01, &read01);
	if (ret)
		goto fail;
	value01 = (read01 & 0x0f) | 0x60;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_01_bank2_or60",
					     0x01, value01);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_04_bank2",
					    0x04, &read04);
	if (ret)
		goto fail;
	value04 = (read04 | 0x01) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_04_bank2_or01",
					     0x04, value04);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_06_bank2_08",
					     0x06, 0x08);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_09_bank2",
					    0x09, &read09);
	if (ret)
		goto fail;
	value09 = (read09 | 0x20) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_09_bank2_or20",
					     0x09, value09);
	if (ret)
		goto fail;

	ret = hd60pro_pipeline_write8_locked(hd, s, "select_bank0_for_24d2ec_head",
					     0x00, 0x00);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_54_bank0",
					    0x54, &read54);
	if (ret)
		goto fail;
	value54 = read54 & 0xef;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_54_bank0_andef",
					     0x54, value54);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_ac_bank0",
					    0xac, &readac);
	if (ret)
		goto fail;
	valueac = (readac | 0x80) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_ac_bank0_or80",
					     0xac, valueac);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_00_bank0_for_or80",
					    0x00, &read00_set);
	if (ret)
		goto fail;
	value00_set = (read00_set | 0x80) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_00_bank0_or80",
					     0x00, value00_set);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_ce_bank0",
					    0xce, &readce);
	if (ret)
		goto fail;
	valuece = (readce | 0x80) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_ce_bank0_or80",
					     0xce, valuece);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_cf_bank0",
					    0xcf, &readcf);
	if (ret)
		goto fail;
	valuecf = ((readcf & 0xfa) | 0x02) & 0xff;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_cf_bank0_andfa_or02",
					     0xcf, valuecf);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_00_bank0_for_and7f",
					    0x00, &read00_clear);
	if (ret)
		goto fail;
	value00_clear = read00_clear & 0x7f;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_00_bank0_and7f",
					     0x00, value00_clear);
	if (ret)
		goto fail;

	goto out_unlock;

fail:
	final_ret = ret;

out_unlock:
	if (!final_ret && hd60pro_mailbox_dead(base)) {
		final_ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "bank2: 01 %02x->%02x 04 %02x->%02x 09 %02x->%02x\n",
		   read01 & 0xff, value01 & 0xff, read04 & 0xff,
		   value04 & 0xff, read09 & 0xff, value09 & 0xff);
	seq_printf(s, "bank0: 54 %02x->%02x ac %02x->%02x 00 %02x->%02x->%02x ce %02x->%02x cf %02x->%02x\n",
		   read54 & 0xff, value54 & 0xff, readac & 0xff,
		   valueac & 0xff, read00_set & 0xff, value00_set & 0xff,
		   value00_clear & 0xff, readce & 0xff, valuece & 0xff,
		   readcf & 0xff, valuecf & 0xff);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table6_local);

static int hd60pro_post_logo_pipeline_24dc28_table7_local_default_show(struct seq_file *s,
								       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	static const u8 windows_table[6] = { 0x06, 0x00, 0x04, 0x03, 0x07, 0x01 };
	u32 read_d0 = 0;
	u32 read_cf = 0;
	u8 table_byte = windows_table[0];
	u8 value_d0 = 0;
	u8 value_cf = 0;
	int final_ret = 0;
	int ret;

	seq_puts(s, "windows_post_logo_pipeline_24dc28_table7_local_default\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024d4db..0x14024d5b2 local/default tail of 0x14024d2ec\n");
	seq_puts(s, "local assumptions: context+0x9810=0, context+0x81dc=0 => table byte 0x06 from {06,00,04,03,07,01}\n");
	seq_puts(s, "planned: read bank0 9c:d0/cf, write d0=(((table>>1)^d0)&3)^d0, write cf=((table<<7)&ff)|(cf&7f)\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table6_local\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (hd60pro_24dc28_state_at_or_after(base, 7)) {
		seq_puts(s, "already_completed: table7 24dc28 state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0xcf &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final table7 state bank0 9c:cf updated observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x00 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table6_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);

	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_d0_bank0",
					    0xd0, &read_d0);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_read8_locked(hd, s, "read_9c_cf_bank0",
					    0xcf, &read_cf);
	if (ret)
		goto fail;

	value_d0 = ((((table_byte >> 1) ^ (read_d0 & 0xff)) & 0x03) ^
		    (read_d0 & 0xff)) & 0xff;
	value_cf = (((table_byte << 7) & 0xff) | (read_cf & 0x7f)) & 0xff;

	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_d0_bank0_local_default",
					     0xd0, value_d0);
	if (ret)
		goto fail;
	ret = hd60pro_pipeline_write8_locked(hd, s, "write_9c_cf_bank0_local_default",
					     0xcf, value_cf);
	if (ret)
		goto fail;

	goto out_unlock;

fail:
	final_ret = ret;

out_unlock:
	if (!final_ret && hd60pro_mailbox_dead(base)) {
		final_ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "table_byte: 0x%02x\n", table_byte);
	seq_printf(s, "d0: 0x%02x -> 0x%02x cf: 0x%02x -> 0x%02x\n",
		   read_d0 & 0xff, value_d0, read_cf & 0xff, value_cf);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24dc28_table7_local_default);

static int hd60pro_post_logo_pipeline_28548c_after_24dc28_tail_show(struct seq_file *s,
								    void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	static const struct {
		const char *name;
		u32 addr;
		u32 reg;
		u32 value;
	} writes[] = {
		{ "write_9a_31_after_24dc28", 0x9a, 0x31, 0x01 },
		{ "write_88_03_a7_after_24dc28", 0x88, 0x03, 0xa7 },
	};
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_28548c_after_24dc28_tail\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14028566b..0x14028568b after local 0x14024dc28 return\n");
	seq_puts(s, "local assumptions: branch from 0x14028559f with context+0x73a8!=4; esi=0, r12=1\n");
	seq_puts(s, "planned: helper 0x1402851cc writes 9a:31=1 then 88:03=0xa7\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24dc28_table7_local_default\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x44 &&
	    ioread32(base + 0x00c) == 0x03 &&
	    ioread32(base + 0x010) == 0xa7 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final after-24dc28 tail state 88:03=0xa7 observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    (ioread32(base + 0x00c) == 0x1f ||
	     ioread32(base + 0x00c) == 0xa9) &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: local no-op path has no mailbox writes for this state\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0xcf ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24dc28_table7_local_default\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		u32 packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			writes[i].addr,
			writes[i].reg,
			writes[i].value,
		};
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   writes[i].name, packet[0], packet[1], packet[2],
			   packet[3], packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_28548c_after_24dc28_tail);

static int hd60pro_post_logo_pipeline_286734_local_noop_show(struct seq_file *s,
							     void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);

	seq_puts(s, "windows_post_logo_pipeline_286734_local_noop\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x140286734 local no-op path, used inside 0x14028548c and again at 0x14027a4cb\n");
	seq_puts(s, "local assumptions: PCI config bytes 0x0e=0 and 0x0f=0 take the 0x140286ca9..0x1402868cf return path\n");
	seq_puts(s, "planned: no mailbox writes for this local path; keep ordering before 0x140287224\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 after validated post_logo_pipeline_28548c_after_24dc28_tail\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if ((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	     ioread32(base + 0x008) == 0x4e &&
	     (ioread32(base + 0x00c) == 0x1f ||
	      ioread32(base + 0x00c) == 0xa9) &&
	     ioread32(base + 0x02c) == 1) ||
	    (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_GPIO_ALT_SET &&
	     ioread32(base + 0x008) == BIT(11) &&
	     ioread32(base + 0x00c) == 0 &&
	     ioread32(base + 0x02c) == 1)) {
		seq_puts(s, "already_completed: later 287224/coefficient state observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x44 ||
	    ioread32(base + 0x00c) != 0x03 ||
	    ioread32(base + 0x010) != 0xa7 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_28548c_after_24dc28_tail or 28548c_tail_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	seq_puts(s, "result: 0\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_286734_local_noop);

static int hd60pro_post_logo_pipeline_28548c_local_prefix_show(struct seq_file *s,
							       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PIPELINE_WRITE,
		0x88,
		0x03,
		0xa7,
	};
	u32 completion = 0;
	int ret;

	seq_puts(s, "windows_post_logo_pipeline_28548c_local_prefix\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS true 0x14028548c local path via helper 0x1402851cc before internal 0x140287224\n");
	seq_puts(s, "local assumptions: context+0x73a8=0, PCI config bytes 0x0e/0x0f are 0; adjacent 0x14024dc28 block is partially reproduced through table2\n");
	seq_puts(s, "planned packet: [0x800,0x1b,0x88,0x03,0xa7]\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_28548c_min\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0x1f &&
	    ioread32(base + 0x010) == 0x01 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final 287224 state 9c:1f=1 observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0xa9 &&
	    ioread32(base + 0x010) == 0x00 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: later coefficient state 9c:a9 low byte observed\n");
		return 0;
	}

	if (!((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x18 &&
	       ioread32(base + 0x010) == 0x00 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x13 &&
	       ioread32(base + 0x010) == 0x08 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x19 &&
	       ioread32(base + 0x010) == 0x02 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x08 &&
	       ioread32(base + 0x010) == 0x03 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0xb4 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x2e &&
	       ioread32(base + 0x02c) == 1))) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_28548c_min, 24dc28_head_local, 24dc28_table1_local, 24dc28_table2_local, 24dc28_table3_local, or 24dc28_table4_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &completion);
	seq_printf(s, "packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
		   packet[0], packet[1], packet[2], packet[3], packet[4],
		   ret, completion, ioread32(base + 0x008),
		   ioread32(base + 0x00c), ioread32(base + 0x010));
	if (!ret && hd60pro_mailbox_dead(base)) {
		ret = -ENODEV;
		seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_28548c_local_prefix);

static int hd60pro_post_logo_shadow_probe_show(struct seq_file *s,
					       void *unused)
{
	static const u32 bank0_regs[] = {
		0x00, 0x18, 0x1e, 0x1f, 0x27, 0x54,
		0xab, 0xac, 0xad, 0xce, 0xcf, 0xd0,
	};
	static const u32 bank2_regs[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
		0x07, 0x09, 0x17, 0x19, 0x1a, 0x1b, 0x1c,
		0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x25,
		0x27, 0x2e, 0x4a,
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_shadow_probe\n");
	seq_puts(s, "purpose: read hardware 9c bank state before 0x140287224/0x14024c894 coefficient assumptions\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (!((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x44 &&
	       ioread32(base + 0x00c) == 0x03 &&
	       ioread32(base + 0x010) == 0xa7 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x1f &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0xa9 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_READ &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x4a &&
	       ioread32(base + 0x02c) == 1))) {
		seq_puts(s, "blocked; run after post_logo_pipeline_28548c_after_24dc28_tail or later post-logo 9c state\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	final_ret = hd60pro_pipeline_write8_locked(hd, s,
						   "select_bank0_for_probe",
						   0x00, 0x00);
	if (final_ret)
		goto out_unlock;

	seq_puts(s, "bank0_values:");
	for (i = 0; i < ARRAY_SIZE(bank0_regs); i++) {
		u32 value = 0;
		char name[32];
		int ret;

		snprintf(name, sizeof(name), "read_bank0_9c_%02x",
			 bank0_regs[i]);
		ret = hd60pro_pipeline_read8_locked(hd, s, name,
						    bank0_regs[i], &value);
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}
		seq_printf(s, "bank0_9c_%02x: 0x%02x\n",
			   bank0_regs[i], value & 0xff);
	}

	final_ret = hd60pro_pipeline_write8_locked(hd, s,
						   "select_bank2_for_probe",
						   0x00, 0x02);
	if (final_ret)
		goto out_unlock;

	seq_puts(s, "bank2_values:");
	for (i = 0; i < ARRAY_SIZE(bank2_regs); i++) {
		u32 value = 0;
		char name[32];
		int ret;

		snprintf(name, sizeof(name), "read_bank2_9c_%02x",
			 bank2_regs[i]);
		ret = hd60pro_pipeline_read8_locked(hd, s, name,
						    bank2_regs[i], &value);
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}
		seq_printf(s, "bank2_9c_%02x: 0x%02x\n",
			   bank2_regs[i], value & 0xff);
	}

out_unlock:
	mutex_unlock(&hd->mailbox_lock);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_shadow_probe);

static int hd60pro_post_logo_pipeline_287224_min_show(struct seq_file *s,
						      void *unused)
{
	static const struct {
		const char *name;
		u32 reg;
		u32 value;
	} tail_writes[] = {
		{ "ad_local_fifth_arg_zero_maps_07", 0xad, 0x07 },
		{ "1e_default_bl", 0x1e, 0x11 },
		{ "1f_default_fifth_arg", 0x1f, 0x01 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 read4a = 0;
	u32 readab = 0;
	u32 readac = 0;
	u32 valueab;
	u32 valueac;
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_287224_min\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x140287224 local/default branch into 0x14024c894, used inside 0x14028548c and again at 0x14027a4d9\n");
	seq_puts(s, "local assumptions: context+0x73a8=0, context+0x9920=0, context+0x9934=0, context+0x81b8=0, defaults 0x80/0x80/0x80/0x80/0x20\n");
	seq_puts(s, "planned packets: bank2 read 9c:4a, bank0 read/modify/write 9c:ab and 9c:ac, then writes 9c:ad=0x07 9c:1e=0x11 9c:1f=1\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_28548c_min\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0x1f &&
	    ioread32(base + 0x010) == 0x01 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final 287224 state 9c:1f=1 observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0xa9 &&
	    ioread32(base + 0x010) == 0x00 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: later coefficient state 9c:a9 low byte observed\n");
		return 0;
	}

	if (!((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x18 &&
	       ioread32(base + 0x010) == 0x00 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x44 &&
	       ioread32(base + 0x00c) == 0x03 &&
	       ioread32(base + 0x010) == 0xa7 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_READ &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x4a &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_GPIO_ALT_SET &&
	       ioread32(base + 0x008) == BIT(11) &&
	       ioread32(base + 0x00c) == 0 &&
	       ioread32(base + 0x02c) == 1))) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_28548c_min or 28548c_tail_local\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	{
		u32 bank2_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			0x00,
			0x02,
		};
		u32 read4a_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_READ8,
			0x9c,
			0x4a,
			0x00,
		};
		u32 bank0_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			0x00,
			0x00,
		};
		u32 readab_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_READ8,
			0x9c,
			0xab,
			0x00,
		};
		u32 readac_packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_READ8,
			0x9c,
			0xac,
			0x00,
		};
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, bank2_packet,
						  ARRAY_SIZE(bank2_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "bank2 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   bank2_packet[0], bank2_packet[1], bank2_packet[2],
			   bank2_packet[3], bank2_packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		ret = hd60pro_mailbox_send_locked(hd, read4a_packet,
						  ARRAY_SIZE(read4a_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		read4a = ioread32(base + 0x010);
		seq_printf(s, "read_9c_4a_bank2 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x value=0x%08x derived_bits_3_2=0x%x bar0_008=0x%08x bar0_00c=0x%08x\n",
			   read4a_packet[0], read4a_packet[1],
			   read4a_packet[2], read4a_packet[3],
			   read4a_packet[4], ret, completion, read4a,
			   (read4a >> 2) & 3, ioread32(base + 0x008),
			   ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		ret = hd60pro_mailbox_send_locked(hd, bank0_packet,
						  ARRAY_SIZE(bank0_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "bank0 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   bank0_packet[0], bank0_packet[1], bank0_packet[2],
			   bank0_packet[3], bank0_packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		ret = hd60pro_mailbox_send_locked(hd, readab_packet,
						  ARRAY_SIZE(readab_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		readab = ioread32(base + 0x010);
		seq_printf(s, "read_9c_ab_bank0 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x value=0x%08x bar0_008=0x%08x bar0_00c=0x%08x\n",
			   readab_packet[0], readab_packet[1],
			   readab_packet[2], readab_packet[3],
			   readab_packet[4], ret, completion, readab,
			   ioread32(base + 0x008), ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		valueab = (readab & 0x95) | 0x15;
		{
			u32 writeab_packet[5] = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0xab,
				valueab,
			};

			ret = hd60pro_mailbox_send_locked(hd, writeab_packet,
							  ARRAY_SIZE(writeab_packet),
							  HD60PRO_MBOX_TIMEOUT_US,
							  &completion);
			seq_printf(s, "write_9c_ab_bank0 value=(read&0x95)|0x15=0x%02x packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
				   valueab & 0xff, writeab_packet[0],
				   writeab_packet[1], writeab_packet[2],
				   writeab_packet[3], writeab_packet[4], ret,
				   completion, ioread32(base + 0x008),
				   ioread32(base + 0x00c),
				   ioread32(base + 0x010));
			if (ret) {
				final_ret = ret;
				goto out_unlock;
			}
		}

		ret = hd60pro_mailbox_send_locked(hd, readac_packet,
						  ARRAY_SIZE(readac_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		readac = ioread32(base + 0x010);
		seq_printf(s, "read_9c_ac_bank0 packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x value=0x%08x bar0_008=0x%08x bar0_00c=0x%08x\n",
			   readac_packet[0], readac_packet[1],
			   readac_packet[2], readac_packet[3],
			   readac_packet[4], ret, completion, readac,
			   ioread32(base + 0x008), ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			goto out_unlock;
		}

		valueac = (readac & 0xd5) | 0x15;
		{
			u32 writeac_packet[5] = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0xac,
				valueac,
			};

			ret = hd60pro_mailbox_send_locked(hd, writeac_packet,
							  ARRAY_SIZE(writeac_packet),
							  HD60PRO_MBOX_TIMEOUT_US,
							  &completion);
			seq_printf(s, "write_9c_ac_bank0 value=(read&0xd5)|0x15=0x%02x packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
				   valueac & 0xff, writeac_packet[0],
				   writeac_packet[1], writeac_packet[2],
				   writeac_packet[3], writeac_packet[4], ret,
				   completion, ioread32(base + 0x008),
				   ioread32(base + 0x00c),
				   ioread32(base + 0x010));
			if (ret) {
				final_ret = ret;
				goto out_unlock;
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(tail_writes); i++) {
		u32 packet[5] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_PIPELINE_WRITE,
			0x9c,
			tail_writes[i].reg,
			tail_writes[i].value,
		};
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "write_tail%u_%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   i, tail_writes[i].name, packet[0], packet[1],
			   packet[2], packet[3], packet[4], ret, completion,
			   ioread32(base + 0x008), ioread32(base + 0x00c),
			   ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}

out_unlock:
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_287224_min);

static int hd60pro_pipeline_write_coeff_locked(struct hd60pro_dev *hd,
					       struct seq_file *s,
					       const char *name, u32 reg,
					       u16 value)
{
	void __iomem *base = hd60pro_mailbox_base(hd);
	u32 high_packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PIPELINE_WRITE,
		0x9c,
		reg + 1,
		(value >> 8) & 0x7f,
	};
	u32 low_packet[5] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_PIPELINE_WRITE,
		0x9c,
		reg,
		value & 0xff,
	};
	u32 high_completion = 0;
	u32 low_completion = 0;
	int ret;

	ret = hd60pro_mailbox_send_locked(hd, high_packet,
					  ARRAY_SIZE(high_packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &high_completion);
	if (ret)
		return ret;

	ret = hd60pro_mailbox_send_locked(hd, low_packet,
					  ARRAY_SIZE(low_packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &low_completion);
	seq_printf(s, "%s reg=0x%02x value=0x%04x high_ret=%d high_done=0x%08x low_ret=%d low_done=0x%08x final_reg=0x%08x final_value=0x%08x\n",
		   name, reg, value, 0, high_completion, ret, low_completion,
		   ioread32(base + 0x00c), ioread32(base + 0x010));

	return ret;
}

static int hd60pro_post_logo_pipeline_24c894_coeffs_show(struct seq_file *s,
							 void *unused)
{
	static const struct {
		const char *name;
		u32 reg;
		u16 value;
	} coeffs[] = {
		{ "coeff0_9b", 0x9b, 0x103f },
		{ "coeff1_95", 0x95, 0x0000 },
		{ "coeff2_a1", 0xa1, 0x0000 },
		{ "coeff3_99", 0x99, 0x0000 },
		{ "coeff4_93", 0x93, 0x1000 },
		{ "coeff5_9f", 0x9f, 0x0000 },
		{ "coeff6_9d", 0x9d, 0x0000 },
		{ "coeff7_97", 0x97, 0x0000 },
		{ "coeff8_a3", 0xa3, 0x1000 },
		{ "tail_a5", 0xa5, 0x2000 },
		{ "tail_a7_local_r14_0", 0xa7, 0x0000 },
		{ "tail_a9", 0xa9, 0x2000 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_24c894_coeffs\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14024cbf1..0x14024cdf2 and helper 0x140274aec\n");
	seq_puts(s, "local assumptions: post_logo_pipeline_287224_min completed, read_9c_4a_bank2=0, defaults 0x80/0x82/0x80 produce fixed coefficient words\n");
	seq_puts(s, "helper 0x140274aec writes [reg+1]=(value>>8)&0x7f, then [reg]=value&0xff via command 0x1b address 0x9c\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_287224_min\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	    ioread32(base + 0x008) == 0x4e &&
	    ioread32(base + 0x00c) == 0xa9 &&
	    ioread32(base + 0x010) == 0x00 &&
	    ioread32(base + 0x02c) == 1) {
		seq_puts(s, "already_completed: final coefficient state 9c:a9 low byte observed\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0x1f ||
	    ioread32(base + 0x010) != 0x01 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_287224_min\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(coeffs); i++) {
		int ret;

		ret = hd60pro_pipeline_write_coeff_locked(hd, s,
							  coeffs[i].name,
							  coeffs[i].reg,
							  coeffs[i].value);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_24c894_coeffs);

static int hd60pro_post_logo_pipeline_28548c_tail_local_show(struct seq_file *s,
							     void *unused)
{
	static const struct {
		const char *name;
		u32 packet[5];
		u8 words;
	} steps[] = {
		{
			.name = "select_bank2_for_9c",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0x00,
				0x02,
			},
			.words = 5,
		},
		{
			.name = "write_9c_27_local_default",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_PIPELINE_WRITE,
				0x9c,
				0x27,
				0x00,
			},
			.words = 5,
		},
		{
			.name = "gpio15_alt_line10_value1",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_GPIO_ALT_SET,
				BIT(10),
				BIT(10),
				0x00,
			},
			.words = 4,
		},
		{
			.name = "gpio15_alt_line11_value0",
			.packet = {
				HD60PRO_MBOX_DOORBELL,
				HD60PRO_MBOX_CMD_GPIO_ALT_SET,
				BIT(11),
				0x00,
				0x00,
			},
			.words = 4,
		},
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_pipeline_28548c_tail_local\n");
	seq_puts(s, "source: local/default tail of e60MZ0380.X64.SYS 0x14028548c after coefficient setup\n");
	seq_puts(s, "local assumptions: context+0x73bc=0, context+0x73a8=0, pci class/revision bytes 0x0e/0x0f are 0\n");
	seq_puts(s, "planned packets: 9c:00=2, 9c:27=0, GPIO15-alt line10=1, GPIO15-alt line11=0\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1 after validated post_logo_pipeline_24c894_coeffs\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_PIPELINE_WRITE ||
	    ioread32(base + 0x008) != 0x4e ||
	    ioread32(base + 0x00c) != 0xa9 ||
	    ioread32(base + 0x010) != 0x00 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_pipeline_24c894_coeffs\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, steps[i].packet,
						  steps[i].words,
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "%s packet=", steps[i].name);
		if (steps[i].words == 4)
			seq_printf(s, "0x%08x,0x%08x,0x%08x,0x%08x",
				   steps[i].packet[0], steps[i].packet[1],
				   steps[i].packet[2], steps[i].packet[3]);
		else
			seq_printf(s, "0x%08x,0x%08x,0x%08x,0x%08x,0x%08x",
				   steps[i].packet[0], steps[i].packet[1],
				   steps[i].packet[2], steps[i].packet[3],
				   steps[i].packet[4]);
		seq_printf(s, " result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   ret, completion, ioread32(base + 0x008),
			   ioread32(base + 0x00c), ioread32(base + 0x010));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_pipeline_28548c_tail_local);

static int hd60pro_post_logo_selector_shadow_probe_show(struct seq_file *s,
							void *unused)
{
	static const struct {
		u32 selector;
		u32 reg;
		const char *name;
	} reads[] = {
		{ 0x30, 0x00, "generic_30_00" },
		{ 0x30, 0x1b, "generic_30_1b" },
		{ 0x88, 0x03, "generic_88_03" },
		{ 0x88, 0x07, "generic_88_07" },
		{ 0x88, 0x0c, "generic_88_0c" },
		{ 0x88, 0x1c, "generic_88_1c" },
		{ 0x88, 0x55, "generic_88_55" },
		{ 0x94, 0x00, "generic_94_00" },
		{ 0x94, 0x07, "generic_94_07" },
		{ 0x9a, 0x00, "generic_9a_00" },
		{ 0x9a, 0x01, "generic_9a_01" },
		{ 0x9a, 0x31, "generic_9a_31" },
		{ 0x9a, 0x38, "generic_9a_38" },
		{ 0xb8, 0x42, "generic_b8_42" },
		{ 0xb8, 0x8c, "generic_b8_8c" },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_selector_shadow_probe\n");
	seq_puts(s, "purpose: read generic selector/register state touched by 0x14028548c through helper 0x1402851cc\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_post_logo_pipeline) {
		seq_puts(s, "blocked; reload with allow_post_logo_pipeline=1 allow_mailbox_writes=1\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required for mailbox reads in this diagnostic\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (!((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_GPIO_ALT_SET &&
	       ioread32(base + 0x008) == BIT(11) &&
	       ioread32(base + 0x00c) == 0 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0xa9 &&
	       ioread32(base + 0x010) == 0x00 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_I2C_WRITE_EXT &&
	       ioread32(base + 0x008) == 0x51 &&
	       ioread32(base + 0x00c) == 0x10 &&
	       ioread32(base + 0x010) == 0x01 &&
	       ioread32(base + 0x02c) == 1))) {
		seq_puts(s, "blocked; run after post_logo_pipeline_28548c_tail_local, 24c894_coeffs, or post_logo_cmd1d_a2\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(reads); i++) {
		u32 value = 0;
		int ret;

		ret = hd60pro_selector_read8_locked(hd, s, reads[i].name,
						    reads[i].selector,
						    reads[i].reg, &value);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_selector_shadow_probe);

static int hd60pro_post_logo_cmd1d_a2_show(struct seq_file *s, void *unused)
{
	static const struct {
		const char *name;
		u32 device;
		u32 reg;
		u32 value;
	} writes[] = {
		{ "a2_11_context_1d0b0_default0", 0xa2, 0x11, 0x00 },
		{ "a2_12_5a", 0xa2, 0x12, 0x5a },
		{ "a2_10_5a", 0xa2, 0x10, 0x5a },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_cmd1d_a2\n");
	seq_puts(s, "source: 0x140287b54, three calls immediately after post-logo pipeline setup\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_cmd1d_write) {
		seq_puts(s, "blocked; reload with allow_cmd1d_write=1 allow_mailbox_writes=1 after validated logo_upload_all\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (!((ioread32(base + 0x004) == HD60PRO_MBOX_CMD_LOGO_COMMIT &&
	       ioread32(base + 0x008) == 1 &&
	       ioread32(base + 0x00c) == HD60PRO_LOGO_PAYLOAD_SIZE &&
	       ioread32(base + 0x010) == HD60PRO_LOGO_DESCRIPTOR &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x18 &&
	       ioread32(base + 0x010) == 0x00 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0x1f &&
	       ioread32(base + 0x010) == 0x01 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_WRITE &&
	       ioread32(base + 0x008) == 0x4e &&
	       ioread32(base + 0x00c) == 0xa9 &&
	       ioread32(base + 0x010) == 0x00 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_GPIO_ALT_SET &&
	       ioread32(base + 0x008) == BIT(11) &&
	       ioread32(base + 0x00c) == 0 &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_PIPELINE_READ &&
	       ioread32(base + 0x008) == 0x5c &&
	       ioread32(base + 0x00c) == 0x8c &&
	       ioread32(base + 0x02c) == 1) ||
	      (ioread32(base + 0x004) == HD60PRO_MBOX_CMD_SELECTOR_READ &&
	       ioread32(base + 0x008) == ((1u << 16) | (0x8c << 8) | 0xb8) &&
	       ioread32(base + 0x02c) == 1))) {
		seq_puts(s, "blocked; last mailbox state does not look like completed logo_upload_all commit or post-logo pipeline step\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(writes); i++) {
		u32 packet[6] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			writes[i].device,
			writes[i].reg,
			1,
			writes[i].value,
		};
		u32 completion = 0;
		u32 status0;
		u32 status1;
		u32 status2;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		status0 = ioread32(base + 0x008);
		status1 = ioread32(base + 0x00c);
		status2 = ioread32(base + 0x010);
		seq_printf(s, "write%u_%s packet=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   i, writes[i].name, packet[0], packet[1], packet[2],
			   packet[3], packet[4], packet[5], ret, completion,
			   status0, status1, status2);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_cmd1d_a2);

static int hd60pro_post_logo_cmd1d_variant_sweep_show(struct seq_file *s,
						      void *unused)
{
	static const u32 a2_11_values[] = { 0x00, 0x01, 0x05, 0x5a, 0xff };
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u8 challenge_bytes[4] = { 0x12, 0x34, 0x56, 0x78 };
	u32 challenge_value = ((u32)challenge_bytes[3] << 24) |
			      ((u32)challenge_bytes[2] << 16) |
			      ((u32)challenge_bytes[1] << 8) |
			      challenge_bytes[0];
	u32 expected_response = hd60pro_windows_challenge_hash(challenge_bytes);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_cmd1d_variant_sweep\n");
	seq_puts(s, "purpose: test context+0x1d0b0 candidates for a2:11 before fixed a2:12/a2:10 and a2:13 challenge\n");
	seq_printf(s, "expected_response: 0x%08x\n", expected_response);
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_cmd1d_write) {
		seq_puts(s, "blocked; reload with allow_cmd1d_write=1 allow_mailbox_writes=1 and run post_logo_cmd1d_a2 first\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_I2C_WRITE_EXT ||
	    ioread32(base + 0x008) != 0x51 ||
	    ioread32(base + 0x00c) != 0x10 ||
	    ioread32(base + 0x010) != 0x01 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; run post_logo_cmd1d_a2 immediately before this sweep\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(a2_11_values); i++) {
		u32 packets[][6] = {
			{ HD60PRO_MBOX_DOORBELL, HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			  0xa2, 0x11, 1, a2_11_values[i] },
			{ HD60PRO_MBOX_DOORBELL, HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			  0xa2, 0x12, 1, 0x5a },
			{ HD60PRO_MBOX_DOORBELL, HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			  0xa2, 0x10, 1, 0x5a },
			{ HD60PRO_MBOX_DOORBELL, HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			  0xa2, 0x13, 8, challenge_value },
		};
		u32 version_packet[3] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_GET_VERSION,
			HD60PRO_MBOX_SELECTOR_FW_A3,
		};
		u32 completion = 0;
		u32 response;
		unsigned int j;
		int ret = 0;

		for (j = 0; j < ARRAY_SIZE(packets); j++) {
			ret = hd60pro_mailbox_send_locked(hd, packets[j],
							  ARRAY_SIZE(packets[j]),
							  HD60PRO_MBOX_TIMEOUT_US,
							  &completion);
			if (ret)
				break;
		}
		if (!ret)
			ret = hd60pro_mailbox_send_locked(hd, version_packet,
							  ARRAY_SIZE(version_packet),
							  HD60PRO_MBOX_TIMEOUT_US,
							  &completion);
		response = ioread32(base + 0x010);
		seq_printf(s, "a2_11=0x%02x ret=%d completion=0x%08x response=0x%08x match=%d bar0_008=0x%08x bar0_00c=0x%08x\n",
			   a2_11_values[i], ret, completion, response,
			   response == expected_response, ioread32(base + 0x008),
			   ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_cmd1d_variant_sweep);

static int hd60pro_post_logo_challenge_a2_show(struct seq_file *s,
					       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u8 challenge_bytes[4] = { 0x12, 0x34, 0x56, 0x78 };
	u32 challenge_value = ((u32)challenge_bytes[3] << 24) |
			      ((u32)challenge_bytes[2] << 16) |
			      ((u32)challenge_bytes[1] << 8) |
			      challenge_bytes[0];
	u32 challenge_packet[6] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
		0xa2,
		0x13,
		8,
		challenge_value,
	};
	u32 version_packet[3] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_GET_VERSION,
		HD60PRO_MBOX_SELECTOR_FW_A3,
	};
	u32 challenge_completion = 0;
	u32 version_completion = 0;
	u32 expected_response = hd60pro_windows_challenge_hash(challenge_bytes);
	u32 challenge_status0;
	u32 challenge_status1;
	u32 challenge_status2;
	u32 version_status0;
	u32 version_status1;
	u32 version_status2;
	int ret;

	seq_puts(s, "windows_post_logo_challenge_a2\n");
	seq_puts(s, "source: 0x14027a534..0x14027a63f, 0x140287bd8 then 0x140277c78 selector 0xa3\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "challenge_bytes: %02x:%02x:%02x:%02x\n",
		   challenge_bytes[0], challenge_bytes[1],
		   challenge_bytes[2], challenge_bytes[3]);
	seq_printf(s, "windows_expected_response: 0x%08x\n",
		   expected_response);

	if (!allow_cmd1d_write) {
		seq_puts(s, "blocked; reload with allow_cmd1d_write=1 allow_mailbox_writes=1 and run post_logo_cmd1d_a2 first\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_I2C_WRITE_EXT ||
	    ioread32(base + 0x008) != 0x51 ||
	    ioread32(base + 0x00c) != 0x10 ||
	    ioread32(base + 0x010) != 0x01 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; last mailbox state does not look like completed post_logo_cmd1d_a2 final write\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_locked(hd, challenge_packet,
					  ARRAY_SIZE(challenge_packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &challenge_completion);
	challenge_status0 = ioread32(base + 0x008);
	challenge_status1 = ioread32(base + 0x00c);
	challenge_status2 = ioread32(base + 0x010);
	seq_printf(s, "challenge_packet: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   challenge_packet[0], challenge_packet[1],
		   challenge_packet[2], challenge_packet[3],
		   challenge_packet[4], challenge_packet[5]);
	seq_printf(s, "challenge_result: %d\n", ret);
	seq_printf(s, "challenge_completion: 0x%08x\n", challenge_completion);
	seq_printf(s, "challenge_status_bar0_0x08: 0x%08x\n",
		   challenge_status0);
	seq_printf(s, "challenge_status_bar0_0x0c: 0x%08x\n",
		   challenge_status1);
	seq_printf(s, "challenge_status_bar0_0x10: 0x%08x\n",
		   challenge_status2);
	if (ret)
		goto out_unlock;

	ret = hd60pro_mailbox_send_locked(hd, version_packet,
					  ARRAY_SIZE(version_packet),
					  HD60PRO_MBOX_TIMEOUT_US,
					  &version_completion);
	version_status0 = ioread32(base + 0x008);
	version_status1 = ioread32(base + 0x00c);
	version_status2 = ioread32(base + 0x010);
	seq_printf(s, "version_packet: 0x%08x 0x%08x 0x%08x\n",
		   version_packet[0], version_packet[1], version_packet[2]);
	seq_printf(s, "version_result: %d\n", ret);
	seq_printf(s, "version_completion: 0x%08x\n", version_completion);
	seq_printf(s, "version_status_bar0_0x08: 0x%08x\n",
		   version_status0);
	seq_printf(s, "version_status_bar0_0x0c: 0x%08x\n",
		   version_status1);
	seq_printf(s, "version_status_bar0_0x10: 0x%08x\n",
		   version_status2);
	seq_printf(s, "windows_challenge_match: %d\n",
		   version_status2 == expected_response);
	hd->pipeline_ready = version_status2 == expected_response;
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);

out_unlock:
	mutex_unlock(&hd->mailbox_lock);
	seq_printf(s, "result: %d\n", ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_challenge_a2);

static int hd60pro_post_logo_challenge_wait_a2_show(struct seq_file *s,
						    void *unused)
{
	static const unsigned int waits_ms[] = { 0, 1, 10, 50, 100, 250, 500, 1000 };
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	u8 challenge_bytes[4] = { 0x12, 0x34, 0x56, 0x78 };
	u32 challenge_value = ((u32)challenge_bytes[3] << 24) |
			      ((u32)challenge_bytes[2] << 16) |
			      ((u32)challenge_bytes[1] << 8) |
			      challenge_bytes[0];
	u32 expected = hd60pro_windows_challenge_hash(challenge_bytes);
	u32 challenge_packet[6] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
		0xa2,
		0x13,
		8,
		challenge_value,
	};
	u32 version_packet[3] = {
		HD60PRO_MBOX_DOORBELL,
		HD60PRO_MBOX_CMD_GET_VERSION,
		HD60PRO_MBOX_SELECTOR_FW_A3,
	};
	u32 challenge_completion = 0;
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_challenge_wait_a2\n");
	seq_puts(s, "sends one deterministic a2:13 challenge, then rereads 0x1c/a3 after increasing waits\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "challenge_bytes: %02x:%02x:%02x:%02x\n",
		   challenge_bytes[0], challenge_bytes[1],
		   challenge_bytes[2], challenge_bytes[3]);
	seq_printf(s, "windows_expected_response: 0x%08x\n", expected);

	if (!allow_cmd1d_write) {
		seq_puts(s, "blocked; reload with allow_cmd1d_write=1 allow_mailbox_writes=1 and run post_logo_cmd1d_a2 first\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_I2C_WRITE_EXT ||
	    ioread32(base + 0x008) != 0x51 ||
	    ioread32(base + 0x00c) != 0x10 ||
	    ioread32(base + 0x010) != 0x01 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; run post_logo_cmd1d_a2 immediately before this wait test\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	final_ret = hd60pro_mailbox_send_locked(hd, challenge_packet,
						ARRAY_SIZE(challenge_packet),
						HD60PRO_MBOX_TIMEOUT_US,
						&challenge_completion);
	seq_printf(s, "challenge_packet: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   challenge_packet[0], challenge_packet[1],
		   challenge_packet[2], challenge_packet[3],
		   challenge_packet[4], challenge_packet[5]);
	seq_printf(s, "challenge_result: %d\n", final_ret);
	seq_printf(s, "challenge_completion: 0x%08x\n", challenge_completion);
	if (final_ret)
		goto out_unlock;

	for (i = 0; i < ARRAY_SIZE(waits_ms); i++) {
		u32 completion = 0;
		u32 response;
		int ret;

		if (waits_ms[i])
			msleep(waits_ms[i]);
		ret = hd60pro_mailbox_send_locked(hd, version_packet,
						  ARRAY_SIZE(version_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		response = ioread32(base + 0x010);
		seq_printf(s, "read_after_ms=%u result=%d completion=0x%08x response=0x%08x match=%d bar0_008=0x%08x bar0_00c=0x%08x\n",
			   waits_ms[i], ret, completion, response,
			   response == expected, ioread32(base + 0x008),
			   ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			break;
		}
	}

out_unlock:
	mutex_unlock(&hd->mailbox_lock);
	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_challenge_wait_a2);

static int hd60pro_post_logo_challenge_sweep_a2_show(struct seq_file *s,
						     void *unused)
{
	static const u8 challenges[][4] = {
		{ 0x12, 0x34, 0x56, 0x78 },
		{ 0x78, 0x56, 0x34, 0x12 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x01, 0x02, 0x03, 0x04 },
		{ 0x10, 0x20, 0x30, 0x40 },
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_challenge_sweep_a2\n");
	seq_puts(s, "sends several deterministic 0x1d a2:13 challenges and checks 0x1c/a3 against the Windows software hash\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_cmd1d_write) {
		seq_puts(s, "blocked; reload with allow_cmd1d_write=1 allow_mailbox_writes=1 and run post_logo_cmd1d_a2 first\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_GET_VERSION ||
	    ioread32(base + 0x008) != 0x51 ||
	    ioread32(base + 0x00c) != 0x13 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; run post_logo_challenge_a2 immediately before this sweep\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(challenges); i++) {
		const u8 *bytes = challenges[i];
		u32 challenge_value = ((u32)bytes[3] << 24) |
				      ((u32)bytes[2] << 16) |
				      ((u32)bytes[1] << 8) |
				      bytes[0];
		u32 challenge_packet[6] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			0xa2,
			0x13,
			8,
			challenge_value,
		};
		u32 version_packet[3] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_GET_VERSION,
			HD60PRO_MBOX_SELECTOR_FW_A3,
		};
		u32 challenge_completion = 0;
		u32 version_completion = 0;
		u32 expected = hd60pro_windows_challenge_hash(bytes);
		u32 response;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, challenge_packet,
						  ARRAY_SIZE(challenge_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &challenge_completion);
		if (ret) {
			final_ret = ret;
			seq_printf(s, "challenge%u bytes=%02x:%02x:%02x:%02x write_result=%d write_completion=0x%08x\n",
				   i, bytes[0], bytes[1], bytes[2], bytes[3],
				   ret, challenge_completion);
			break;
		}

		ret = hd60pro_mailbox_send_locked(hd, version_packet,
						  ARRAY_SIZE(version_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &version_completion);
		response = ioread32(base + 0x010);
		seq_printf(s, "challenge%u bytes=%02x:%02x:%02x:%02x packet_value=0x%08x expected=0x%08x response=0x%08x match=%d write_completion=0x%08x read_result=%d read_completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x\n",
			   i, bytes[0], bytes[1], bytes[2], bytes[3],
			   challenge_value, expected, response,
			   response == expected, challenge_completion, ret,
			   version_completion, ioread32(base + 0x008),
			   ioread32(base + 0x00c));
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_challenge_sweep_a2);

static int hd60pro_post_logo_selector1c_dump_show(struct seq_file *s,
						  void *unused)
{
	static const u32 selectors[] = {
		0x50, 0x51, 0x90, 0x98, 0x9a, 0x9c,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
		0xb0, 0xc0,
	};
	struct hd60pro_dev *hd = s->private;
	void __iomem *base = hd60pro_mailbox_base(hd);
	unsigned int i;
	int final_ret = 0;

	seq_puts(s, "windows_post_logo_selector1c_dump\n");
	seq_puts(s, "source: generic Windows read helper 0x140277c78 sends packet [0x800,0x1c,selector] and returns BAR0+0x10\n");
	seq_puts(s, "purpose: inspect nearby selectors after the A2 post-logo writes without running the A2 challenge loop\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());

	if (!allow_cmd1d_write) {
		seq_puts(s, "blocked; reload with allow_cmd1d_write=1 allow_mailbox_writes=1 and run post_logo_cmd1d_a2 first\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	if (ioread32(base + 0x004) != HD60PRO_MBOX_CMD_I2C_WRITE_EXT ||
	    ioread32(base + 0x008) != 0x51 ||
	    ioread32(base + 0x00c) != 0x10 ||
	    ioread32(base + 0x010) != 0x01 ||
	    ioread32(base + 0x02c) != 1) {
		seq_puts(s, "blocked; run post_logo_cmd1d_a2 immediately before this dump\n");
		seq_printf(s, "bar0_004: 0x%08x\n", ioread32(base + 0x004));
		seq_printf(s, "bar0_008: 0x%08x\n", ioread32(base + 0x008));
		seq_printf(s, "bar0_00c: 0x%08x\n", ioread32(base + 0x00c));
		seq_printf(s, "bar0_010: 0x%08x\n", ioread32(base + 0x010));
		seq_printf(s, "bar0_02c: 0x%08x\n", ioread32(base + 0x02c));
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(selectors); i++) {
		u32 packet[3] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_GET_VERSION,
			selectors[i],
		};
		u32 completion = 0;
		u32 status0;
		u32 status1;
		u32 status2;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, packet, ARRAY_SIZE(packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		status0 = ioread32(base + 0x008);
		status1 = ioread32(base + 0x00c);
		status2 = ioread32(base + 0x010);
		seq_printf(s, "selector=0x%02x result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   selectors[i], ret, completion, status0, status1,
			   status2);
		if (ret) {
			final_ret = ret;
			break;
		}
		if (hd60pro_mailbox_dead(base)) {
			final_ret = -ENODEV;
			seq_puts(s, "aborted: mailbox reads indicate inaccessible device\n");
			break;
		}
	}
	if (!final_ret) {
		u32 restore_packet[6] = {
			HD60PRO_MBOX_DOORBELL,
			HD60PRO_MBOX_CMD_I2C_WRITE_EXT,
			0xa2,
			0x10,
			1,
			0x5a,
		};
		u32 completion = 0;
		int ret;

		ret = hd60pro_mailbox_send_locked(hd, restore_packet,
						  ARRAY_SIZE(restore_packet),
						  HD60PRO_MBOX_TIMEOUT_US,
						  &completion);
		seq_printf(s, "restore_a2_10_5a result=%d completion=0x%08x bar0_008=0x%08x bar0_00c=0x%08x bar0_010=0x%08x\n",
			   ret, completion, ioread32(base + 0x008),
			   ioread32(base + 0x00c), ioread32(base + 0x010));
		final_ret = ret;
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "result: %d\n", final_ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_post_logo_selector1c_dump);

static int hd60pro_health_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct pci_dev *pdev = hd->pdev;
	u16 command;
	u16 status;
	u32 bar5_000;
	u32 bar5_02c;
	u32 bar5_030;
	u32 bar5_050;
	void __iomem *mbox = hd60pro_mailbox_base(hd);
	u32 mbox_000;
	u32 mbox_008;
	u32 mbox_00c;
	u32 mbox_02c;

	pci_read_config_word(pdev, PCI_COMMAND, &command);
	pci_read_config_word(pdev, PCI_STATUS, &status);

	mbox_000 = mbox ? ioread32(mbox + 0x000) : U32_MAX;
	mbox_008 = mbox ? ioread32(mbox + 0x008) : U32_MAX;
	mbox_00c = mbox ? ioread32(mbox + 0x00c) : U32_MAX;
	mbox_02c = mbox ? ioread32(mbox + 0x02c) : U32_MAX;
	bar5_000 = hd->bar5 ? ioread32(hd->bar5 + 0x000) : U32_MAX;
	bar5_02c = hd->bar5 ? ioread32(hd->bar5 + 0x02c) : U32_MAX;
	bar5_030 = hd->bar5 ? ioread32(hd->bar5 + 0x030) : U32_MAX;
	bar5_050 = hd->bar5 ? ioread32(hd->bar5 + 0x050) : U32_MAX;

	seq_printf(s, "pci_command: 0x%04x\n", command);
	seq_printf(s, "pci_status: 0x%04x\n", status);
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "streaming: %d\n", hd->streaming);
	seq_printf(s, "dma_desc_allocated: %d\n", hd->dma_desc_cpu != NULL);
	seq_printf(s, "dma_frame_allocated: %d\n", hd->dma_frame_cpu != NULL);
	seq_printf(s, "mailbox_000: 0x%08x\n", mbox_000);
	seq_printf(s, "mailbox_008: 0x%08x\n", mbox_008);
	seq_printf(s, "mailbox_00c: 0x%08x\n", mbox_00c);
	seq_printf(s, "mailbox_02c: 0x%08x\n", mbox_02c);
	seq_printf(s, "mailbox_alive: %d\n",
		   mbox_000 != U32_MAX || mbox_008 != U32_MAX ||
		   mbox_00c != U32_MAX || mbox_02c != U32_MAX);
	seq_printf(s, "bar5_000: 0x%08x\n", bar5_000);
	seq_printf(s, "bar5_02c: 0x%08x\n", bar5_02c);
	seq_printf(s, "bar5_030: 0x%08x\n", bar5_030);
	seq_printf(s, "bar5_050: 0x%08x\n", bar5_050);
	seq_printf(s, "bar5_alive: %d\n",
		   bar5_000 != U32_MAX || bar5_02c != U32_MAX ||
		   bar5_030 != U32_MAX || bar5_050 != U32_MAX);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_health);

static int hd60pro_alloc_diag_dma(struct hd60pro_dev *hd)
{
	struct hd60pro_host_frame_desc *desc;
	unsigned int i;

	hd->dma_desc_size = HD60PRO_DMA_DESC_BYTES;
	hd->dma_frame_size = hd60pro_frame_size();
	hd->win_dma_control_size = HD60PRO_WINDOWS_DMA_CONTROL_BYTES;
	hd->win_dma_status_size = HD60PRO_WINDOWS_DMA_STATUS_BYTES;

	hd->dma_desc_cpu = dma_alloc_coherent(&hd->pdev->dev,
					      hd->dma_desc_size,
					      &hd->dma_desc_dma, GFP_KERNEL);
	if (!hd->dma_desc_cpu)
		return -ENOMEM;

	hd->dma_frame_cpu = dma_alloc_coherent(&hd->pdev->dev,
					       hd->dma_frame_size,
					       &hd->dma_frame_dma, GFP_KERNEL);
	if (!hd->dma_frame_cpu) {
		dma_free_coherent(&hd->pdev->dev, hd->dma_desc_size,
				  hd->dma_desc_cpu, hd->dma_desc_dma);
		hd->dma_desc_cpu = NULL;
		hd->dma_desc_dma = 0;
		hd->dma_desc_size = 0;
		return -ENOMEM;
	}

	hd->win_dma_control_cpu =
		dma_alloc_coherent(&hd->pdev->dev, hd->win_dma_control_size,
				   &hd->win_dma_control_dma, GFP_KERNEL);
	if (!hd->win_dma_control_cpu)
		goto err_free_frame;

	hd->win_dma_status_cpu =
		dma_alloc_coherent(&hd->pdev->dev, hd->win_dma_status_size,
				   &hd->win_dma_status_dma, GFP_KERNEL);
	if (!hd->win_dma_status_cpu)
		goto err_free_control;

	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++) {
		hd->win_dma_channel_size[i] =
			HD60PRO_WINDOWS_DMA_CHANNEL_BYTES;
		hd->win_dma_channel_cpu[i] =
			dma_alloc_coherent(&hd->pdev->dev,
					   hd->win_dma_channel_size[i],
					   &hd->win_dma_channel_dma[i],
					   GFP_KERNEL);
		if (!hd->win_dma_channel_cpu[i])
			goto err_free_channels;
	}

	memset(hd->dma_desc_cpu, 0, hd->dma_desc_size);
	memset(hd->dma_frame_cpu, 0, hd->dma_frame_size);
	memset(hd->win_dma_control_cpu, 0, hd->win_dma_control_size);
	memset(hd->win_dma_status_cpu, 0, hd->win_dma_status_size);
	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++)
		memset(hd->win_dma_channel_cpu[i], 0,
		       hd->win_dma_channel_size[i]);

	desc = hd->dma_desc_cpu;
	desc->magic = cpu_to_le32(HD60PRO_HOST_DESC_MAGIC);
	desc->version = cpu_to_le32(HD60PRO_HOST_DESC_VERSION);
	desc->frame_dma = cpu_to_le64(hd->dma_frame_dma);
	desc->frame_bytes = cpu_to_le32(hd->dma_frame_size);
	desc->width = cpu_to_le32(HD60PRO_DEFAULT_WIDTH);
	desc->height = cpu_to_le32(HD60PRO_DEFAULT_HEIGHT);
	desc->fourcc = cpu_to_le32(V4L2_PIX_FMT_YUYV);
	desc->fps_millihz = cpu_to_le32(60000);
	desc->windows_stream_8144 =
		cpu_to_le32((HD60PRO_DEFAULT_WIDTH << 16) |
			    HD60PRO_DEFAULT_HEIGHT);
	desc->windows_stream_8148 = cpu_to_le32(60);
	desc->windows_stream_814c = cpu_to_le32(0);
	desc->windows_stream_8578 =
		cpu_to_le32(HD60PRO_WINDOWS_STREAM_FPS_60);
	desc->windows_stream_72c0 = cpu_to_le32(1);
	desc->meta_size = cpu_to_le32(sizeof(struct hd60pro_direct_frame_meta));
	return 0;

err_free_channels:
	while (i--) {
		dma_free_coherent(&hd->pdev->dev,
				  hd->win_dma_channel_size[i],
				  hd->win_dma_channel_cpu[i],
				  hd->win_dma_channel_dma[i]);
		hd->win_dma_channel_cpu[i] = NULL;
		hd->win_dma_channel_dma[i] = 0;
		hd->win_dma_channel_size[i] = 0;
	}
	dma_free_coherent(&hd->pdev->dev, hd->win_dma_status_size,
			  hd->win_dma_status_cpu, hd->win_dma_status_dma);
	hd->win_dma_status_cpu = NULL;
	hd->win_dma_status_dma = 0;
	hd->win_dma_status_size = 0;
err_free_control:
	dma_free_coherent(&hd->pdev->dev, hd->win_dma_control_size,
			  hd->win_dma_control_cpu, hd->win_dma_control_dma);
	hd->win_dma_control_cpu = NULL;
	hd->win_dma_control_dma = 0;
	hd->win_dma_control_size = 0;
err_free_frame:
	dma_free_coherent(&hd->pdev->dev, hd->dma_frame_size,
			  hd->dma_frame_cpu, hd->dma_frame_dma);
	hd->dma_frame_cpu = NULL;
	hd->dma_frame_dma = 0;
	hd->dma_frame_size = 0;
	dma_free_coherent(&hd->pdev->dev, hd->dma_desc_size,
			  hd->dma_desc_cpu, hd->dma_desc_dma);
	hd->dma_desc_cpu = NULL;
	hd->dma_desc_dma = 0;
	hd->dma_desc_size = 0;
	return -ENOMEM;
}

static void hd60pro_free_diag_dma(struct hd60pro_dev *hd)
{
	unsigned int i;

	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++) {
		if (hd->win_dma_channel_cpu[i]) {
			dma_free_coherent(&hd->pdev->dev,
					  hd->win_dma_channel_size[i],
					  hd->win_dma_channel_cpu[i],
					  hd->win_dma_channel_dma[i]);
			hd->win_dma_channel_cpu[i] = NULL;
			hd->win_dma_channel_dma[i] = 0;
			hd->win_dma_channel_size[i] = 0;
		}
	}

	if (hd->win_dma_status_cpu) {
		dma_free_coherent(&hd->pdev->dev, hd->win_dma_status_size,
				  hd->win_dma_status_cpu,
				  hd->win_dma_status_dma);
		hd->win_dma_status_cpu = NULL;
		hd->win_dma_status_dma = 0;
		hd->win_dma_status_size = 0;
	}

	if (hd->win_dma_control_cpu) {
		dma_free_coherent(&hd->pdev->dev, hd->win_dma_control_size,
				  hd->win_dma_control_cpu,
				  hd->win_dma_control_dma);
		hd->win_dma_control_cpu = NULL;
		hd->win_dma_control_dma = 0;
		hd->win_dma_control_size = 0;
	}

	if (hd->dma_frame_cpu) {
		dma_free_coherent(&hd->pdev->dev, hd->dma_frame_size,
				  hd->dma_frame_cpu, hd->dma_frame_dma);
		hd->dma_frame_cpu = NULL;
		hd->dma_frame_dma = 0;
		hd->dma_frame_size = 0;
	}

	if (hd->dma_desc_cpu) {
		dma_free_coherent(&hd->pdev->dev, hd->dma_desc_size,
				  hd->dma_desc_cpu, hd->dma_desc_dma);
		hd->dma_desc_cpu = NULL;
		hd->dma_desc_dma = 0;
		hd->dma_desc_size = 0;
	}
}

static int hd60pro_dma_info_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;
	unsigned int i;

	seq_printf(s, "prepare_dma_buffers: %d\n", prepare_dma_buffers);
	seq_printf(s, "dma_mask_bits: %u\n",
		   dma_get_mask(&hd->pdev->dev) == DMA_BIT_MASK(64) ? 64 : 32);
	seq_printf(s, "desc_allocated: %d\n", hd->dma_desc_cpu != NULL);
	seq_printf(s, "desc_cpu: %px\n", hd->dma_desc_cpu);
	seq_printf(s, "desc_dma: %pad\n", &hd->dma_desc_dma);
	seq_printf(s, "desc_size: %zu\n", hd->dma_desc_size);
	seq_printf(s, "frame_allocated: %d\n", hd->dma_frame_cpu != NULL);
	seq_printf(s, "frame_cpu: %px\n", hd->dma_frame_cpu);
	seq_printf(s, "frame_dma: %pad\n", &hd->dma_frame_dma);
	seq_printf(s, "frame_size: %zu\n", hd->dma_frame_size);
	seq_printf(s, "windows_device_0xd0_control_cpu: %px\n",
		   hd->win_dma_control_cpu);
	seq_printf(s, "windows_device_0xc8_control_dma: %pad\n",
		   &hd->win_dma_control_dma);
	seq_printf(s, "windows_device_0xe0_control_size: %zu\n",
		   hd->win_dma_control_size);
	seq_printf(s, "windows_device_0x140_status_cpu: %px\n",
		   hd->win_dma_status_cpu);
	seq_printf(s, "windows_device_0x138_status_dma: %pad\n",
		   &hd->win_dma_status_dma);
	seq_printf(s, "windows_device_0x148_status_size: %zu\n",
		   hd->win_dma_status_size);
	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++) {
		seq_printf(s, "windows_device_channel%u_0x1190_cpu: %px\n",
			   i, hd->win_dma_channel_cpu[i]);
		seq_printf(s, "windows_device_channel%u_0x190_dma: %pad\n",
			   i, &hd->win_dma_channel_dma[i]);
		seq_printf(s, "windows_device_channel%u_size: %zu\n",
			   i, hd->win_dma_channel_size[i]);
	}
	if (desc) {
		seq_printf(s, "host_desc_magic: 0x%08x\n",
			   le32_to_cpu(desc->magic));
		seq_printf(s, "host_desc_version: %u\n",
			   le32_to_cpu(desc->version));
		seq_printf(s, "host_desc_frame_dma: 0x%016llx\n",
			   le64_to_cpu(desc->frame_dma));
		seq_printf(s, "host_desc_frame_bytes: %u\n",
			   le32_to_cpu(desc->frame_bytes));
		seq_printf(s, "host_desc_format: %ux%u fourcc=0x%08x fps_millihz=%u\n",
			   le32_to_cpu(desc->width),
			   le32_to_cpu(desc->height),
			   le32_to_cpu(desc->fourcc),
			   le32_to_cpu(desc->fps_millihz));
		seq_printf(s, "host_desc_windows_stream_8144: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8144));
		seq_printf(s, "host_desc_windows_stream_8148: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8148));
		seq_printf(s, "host_desc_windows_stream_814c: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_814c));
		seq_printf(s, "host_desc_windows_stream_8578: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8578));
		seq_printf(s, "host_desc_windows_stream_72c0: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_72c0));
		seq_printf(s, "host_desc_meta_size: %u\n",
			   le32_to_cpu(desc->meta_size));
	}
	seq_puts(s, "status: buffers only; no DMA registers are programmed by this diagnostic\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_dma_info);

static int hd60pro_capture_info_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u16 command;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "capture_bringup_state: experimental\n");
	seq_printf(s, "pipeline_ready_after_a2: %d\n", hd->pipeline_ready);
	seq_printf(s, "v4l2_registered: %d\n", enable_v4l2);
	if (enable_v4l2)
		seq_printf(s, "video_device: /dev/video%d\n", hd->vdev.num);
	seq_printf(s, "v4l2_streaming_now: %d\n", hd->streaming);
	seq_printf(s, "v4l2_synthetic_frames: %d\n", synthetic_v4l2);
	seq_printf(s, "v4l2_frame_format: YUYV %ux%u 60fps\n",
		   HD60PRO_DEFAULT_WIDTH, HD60PRO_DEFAULT_HEIGHT);
	seq_printf(s, "v4l2_frames_completed: %u\n", hd->sequence);
	seq_printf(s, "last_frame_sequence: %u\n",
		   hd->last_frame_meta.sequence);
	seq_printf(s, "last_frame_timestamp_ns: %llu\n",
		   hd->last_frame_meta.timestamp_ns);
	seq_printf(s, "last_frame_duration_ns: %llu\n",
		   hd->last_frame_meta.duration_ns);
	seq_printf(s, "last_frame_payload_bytes: %u\n",
		   hd->last_frame_meta.payload_bytes);
	seq_printf(s, "last_frame_flags: 0x%08x\n",
		   hd->last_frame_meta.flags);
	seq_printf(s, "pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_printf(s, "irq_requested: %d\n", hd->irq >= 0);
	seq_printf(s, "irq_count: %u\n", hd->irq_count);
	seq_printf(s, "mailbox_irq_count: %u\n", hd->mailbox_irq_count);
	seq_printf(s, "dma_buffers_prepared: %d\n",
		   hd->dma_desc_cpu != NULL && hd->dma_frame_cpu != NULL);
	seq_printf(s, "dma_desc_dma: %pad\n", &hd->dma_desc_dma);
	seq_printf(s, "dma_desc_size: %zu\n", hd->dma_desc_size);
	seq_printf(s, "dma_frame_dma: %pad\n", &hd->dma_frame_dma);
	seq_printf(s, "dma_frame_size: %zu\n", hd->dma_frame_size);
	seq_printf(s, "windows_dma_control_prepared: %d\n",
		   hd->win_dma_control_cpu != NULL);
	seq_printf(s, "windows_dma_status_prepared: %d\n",
		   hd->win_dma_status_cpu != NULL);
	seq_printf(s, "windows_dma_channel0_prepared: %d\n",
		   hd->win_dma_channel_cpu[0] != NULL);
	seq_puts(s, "real_dma_programmed: 0\n");
	seq_puts(s, "next_target: find Windows capture-start DMA descriptor/ring register writes after A2 pipeline-ready state\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_capture_info);

static int hd60pro_endpoint_info_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 bar5_040 = hd->bar5 ? ioread32(hd->bar5 + 0x040) : U32_MAX;
	u32 bar5_044 = hd->bar5 ? ioread32(hd->bar5 + 0x044) : U32_MAX;
	u32 bar5_048 = hd->bar5 ? ioread32(hd->bar5 + 0x048) : U32_MAX;
	u32 bar5_04c = hd->bar5 ? ioread32(hd->bar5 + 0x04c) : U32_MAX;

	seq_puts(s, "endpoint_bridge_hypothesis: firmware-side /sys/vpl_pciep controls host PCI endpoint shared memory and interrupts\n");
	seq_puts(s, "source: embedded MZ0380.HD.HEX yuan_demo_sdi/drivers/ep.ko strings and symbols\n");
	seq_puts(s, "firmware_sysfs_attrs: command firmware logo status epint epint_1080p audio_ctrl hready channel_done\n");
	seq_puts(s, "firmware_attr_strings: /sys/vpl_pciep/logo /sys/vpl_pciep/epint /sys/vpl_pciep/hready /sys/class/vpl_pciep/channel_done\n");
	seq_puts(s, "firmware_commands_seen: BEGIN_FIRMWARE_DOWNLOAD BEGIN_BASE_FIRMWARE_DOWNLOAD SET_VIC_PARAMS STOP_STREAMING GET_FIRMWARE_VERSION\n");
	seq_puts(s, "firmware_set_vic_format: SET_VIC fw,fps,resolution,interlace,color_info,input_frame_width,input_frame_height,bitstream_num,nosg fields\n");
	seq_puts(s, "firmware_stop_streaming_side_effect: writes 0 to /sys/vpl_pciep/hready and kills capture_app_infinite/capture_audio_8ch\n");
	seq_puts(s, "firmware_logo_path: video_capture_mgr allocates logo memory then writes /sys/vpl_pciep/logo\n");
	seq_puts(s, "firmware_interrupt_path: video_capture_mgr opens /sys/vpl_pciep/epint; ep.ko has pciep_isr/store_channel_done/pending_irqs\n");
	seq_puts(s, "firmware_export_seen: pcie_set_outbound\n");
	seq_puts(s, "firmware_command_store_decoded: /sys/vpl_pciep/command accepts command IDs 0x18..0x22, copies payload by ep_cmds_size[cmd], then sets ep_command+0x28 ready\n");
	seq_puts(s, "firmware_pcie_set_outbound_decoded: writes endpoint regs +0x50=1, +0x74=0x90000000, +0x7c=0x91ffffff, +0x54/+0x58=selected outbound address pair, +0xd4=0x00f00000\n");
	seq_puts(s, "firmware_pciep_isr_set_vic: command 0x29 parses ep_command+0x05 fps, +0x06 fw/mode, +0x08 width, +0x0a height, +0x22 interrupt_reduce\n");
	seq_puts(s, "firmware_pciep_isr_stream_notify: command 0x2a notifies audio_ctrl then epint/epint_1080p depending on mode after SET_VIC\n");
	seq_puts(s, "firmware_pciep_isr_frame_notify: commands 0x50..0x52, 0x60..0x62, 0x6e also notify epint/epint_1080p/status paths\n");
	seq_puts(s, "firmware_dma_modules: vpl_dmac.ko vpl_vic.ko vpl_edmc.ko\n");
	seq_puts(s, "firmware_vic_symbols: VideoCap_OpenVIC InitVIC StartVIC WaitVIC GetBufVIC ReleaseBufVIC StopVIC CloseVIC\n");
	seq_puts(s, "firmware_dmac_symbols: VPL_DMAC_SetMMRInfo SetupProfile StartHead StartTail IntrEnable IntrClear Reset\n");
	seq_printf(s, "pipeline_ready_after_a2: %d\n", hd->pipeline_ready);
	seq_printf(s, "host_dma_buffers_prepared: %d\n",
		   hd->dma_desc_cpu != NULL && hd->dma_frame_cpu != NULL);
	seq_printf(s, "host_dma_desc_dma: %pad\n", &hd->dma_desc_dma);
	seq_printf(s, "host_dma_frame_dma: %pad\n", &hd->dma_frame_dma);
	seq_printf(s, "bar5_payload0_0x40: 0x%08x\n", bar5_040);
	seq_printf(s, "bar5_payload1_0x44: 0x%08x\n", bar5_044);
	seq_printf(s, "bar5_payload2_0x48: 0x%08x\n", bar5_048);
	seq_printf(s, "bar5_payload3_0x4c: 0x%08x\n", bar5_04c);
	seq_puts(s, "real_capture_action: none\n");
	seq_puts(s, "next_reverse_target: Windows host call path that corresponds to firmware epint/hready/channel_done and SET_VIC_PARAMS\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_endpoint_info);

static int hd60pro_direct_memory_info_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "windows_direct_memory_property: CustomAnalogVideoDirectMemoryModeProperty\n");
	seq_puts(s, "windows_getter_xref: 0x14024655c reads context+0x8228\n");
	seq_puts(s, "windows_setter_xref: 0x1402465ae writes context+0x8228\n");
	seq_puts(s, "windows_buffer_xref: 0x1402466af copies 16 bytes from context+0x822c to userspace buffer\n");
	seq_puts(s, "windows_frame_helper: 0x1402466bb calls 0x14027eb38 when direct memory mode and stream buffer are present\n");
	seq_puts(s, "windows_helper_status: partially decoded\n");
	seq_puts(s, "windows_context_direct_memory_mode_offset: 0x8228\n");
	seq_puts(s, "windows_context_direct_memory_blob_offset: 0x822c\n");
	seq_puts(s, "windows_context_stream_info_offset: 0x1d110\n");
	seq_puts(s, "windows_context_streaming_flag_offset: 0x72c0\n");
	seq_puts(s, "windows_stream_info_frame_rate_offset: 0x40\n");
	seq_puts(s, "windows_stream_info_flags_offset: 0x48\n");
	seq_puts(s, "windows_stream_info_frame_period_offset: 0x58\n");
	seq_puts(s, "windows_stream_info_format1_offset: 0x1dc\n");
	seq_puts(s, "windows_stream_info_format2_offset: 0x30c\n");
	seq_puts(s, "windows_stream_info_logo_gate_offset: 0x2074\n");
	seq_puts(s, "windows_frame_meta_timestamp_offset: 0x08\n");
	seq_puts(s, "windows_frame_meta_duration_offset: 0x18\n");
	seq_puts(s, "windows_frame_meta_payload_offset: 0x24\n");
	seq_puts(s, "windows_frame_meta_flags_offset: 0x30\n");
	seq_puts(s, "windows_frame_meta_extra_offset: 0x38\n");
	seq_puts(s, "windows_stream_frame_object_meta_pointer_offset: frame_object+0x10\n");
	seq_puts(s, "windows_stream_frame_object_buffer_pointer_offset: frame_object+0x20\n");
	seq_puts(s, "windows_stream_frame_object_payload_or_stride_offset: frame_object+0x28\n");
	seq_puts(s, "windows_stream_frame_object_aux_pointer_offset: frame_object+0x2c\n");
	seq_puts(s, "windows_stream_frame_object_flag_byte_offset: frame_object+0x35\n");
	seq_puts(s, "windows_frame_helper_notes: 0x14027eb38 updates timestamps/frame counters and writes logo/no-signal image data when no real frame is ready\n");
	seq_printf(s, "linux_v4l2_synthetic_frames: %d\n", synthetic_v4l2);
	seq_printf(s, "linux_v4l2_frames_completed: %u\n", hd->sequence);
	seq_printf(s, "linux_pipeline_ready_after_a2: %d\n", hd->pipeline_ready);
	seq_printf(s, "linux_dma_buffers_prepared: %d\n",
		   hd->dma_desc_cpu != NULL && hd->dma_frame_cpu != NULL);
	seq_printf(s, "linux_last_frame_sequence: %u\n",
		   hd->last_frame_meta.sequence);
	seq_printf(s, "linux_last_frame_timestamp_ns: %llu\n",
		   hd->last_frame_meta.timestamp_ns);
	seq_printf(s, "linux_last_frame_duration_ns: %llu\n",
		   hd->last_frame_meta.duration_ns);
	seq_printf(s, "linux_last_frame_payload_bytes: %u\n",
		   hd->last_frame_meta.payload_bytes);
	seq_printf(s, "linux_last_frame_flags: 0x%08x\n",
		   hd->last_frame_meta.flags);
	seq_printf(s, "linux_last_frame_extra: 0x%08x\n",
		   hd->last_frame_meta.extra);
	seq_printf(s, "linux_direct_memory_blob_words: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		   hd->direct_memory_blob[0], hd->direct_memory_blob[1],
		   hd->direct_memory_blob[2], hd->direct_memory_blob[3]);
	seq_puts(s, "linux_direct_memory_real_mode: 0\n");
	seq_puts(s, "next_needed: map Windows direct-memory buffer registration to host PCI endpoint/shared-memory objects\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_direct_memory_info);

static int hd60pro_windows_directmemory_drain_path_show(struct seq_file *s,
							void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "windows_directmemory_drain_path: decoded DirectMemory frame-drain helpers; no hardware writes\n");
	seq_puts(s, "entry_140293c39: locks device+0x1d108, pops frame/list objects, resets frame metadata at frame+0x10, then dispatches by stream/frame type\n");
	seq_puts(s, "entry_1402949xx: sibling drain path calls the same frame helpers and then releases/list-requeues the frame object through 0x1402a50c8/0x1402a5070/0x1402a50e8\n");
	seq_puts(s, "type_dispatch_helpers: frame type groups call 0x14027eb38, 0x14027e3b0, or 0x14027d698 with frame buffer pointer, frame metadata pointer, payload/stride, flags, and stream object state\n");
	seq_puts(s, "frame_metadata_reset_140293c7f: frame+0x10 points to metadata; fields +0x08/+0x18/+0x24 are cleared or rewritten before helper call\n");
	seq_puts(s, "stream_object_inputs: helpers consume device+0x1d110 stream fields +0x40/+0x48/+0x58/+0x1dc/+0x30c/+0x822c plus frame type and timing counters\n");
	seq_puts(s, "software_source_buffer: 0x14027eb38 and related pre-helper code read from device+0xd0 plus offsets 0x0/0x2000/0x4000/0x6000 or interlaced/packed variants\n");
	seq_puts(s, "copy_patterns: helper copies blocks with 0x140001820 into the delivered frame buffer and writes payload bytes such as 0x1000, 0x2000, 0x3c0, or 0x780 depending on format\n");
	seq_puts(s, "timestamp_patterns: helper calls object+0x68 vtable +0x18 through CFG dispatch 0x1402a52d0 to compute cadence/timestamps, then updates frame metadata +0x08/+0x18/+0x24/+0x30\n");
	seq_puts(s, "event_state_siblings: 0x140294c09 and 0x140294e0d load object->+0xc0->+0x10, map frame type to slot 0..0x1e, then call 0x14028fa04 state updater; this is queue state notification, not DMA programming\n");
	seq_puts(s, "important_result: DirectMemory drain currently looks like software frame production/format fallback from Windows contiguous memory, not the PCI endpoint descriptor/ring advertisement itself\n");
	seq_puts(s, "important_negative_result: no writes to BAR0/BAR5, no MmGetPhysicalAddress handoff, and no host physical frame address publication were decoded in this drain path\n");
	seq_puts(s, "linux_model_gap: Linux allocates a coherent frame buffer, but does not yet implement the Windows device+0xd0 software-source layout or the endpoint event-to-frame list objects\n");
	seq_puts(s, "next_static_targets:\n");
	seq_puts(s, "  writers of device+0xd0 contents and any firmware/endpoint copy into that contiguous memory\n");
	seq_puts(s, "  constructors for frame/list objects consumed by 0x140293c39 and sibling drain path\n");
	seq_puts(s, "  first transition that sets streaming flag +0x72c0 and triggers endpoint hready/SET_VIC/channel_done\n");
	seq_puts(s, "linux_status:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  dma_buffers_prepared: %d\n",
		   hd->dma_desc_cpu != NULL && hd->dma_frame_cpu != NULL);
	seq_printf(s, "  v4l2_frames_completed: %u\n", hd->sequence);
	seq_printf(s, "  last_frame_payload_bytes: %u\n",
		   hd->last_frame_meta.payload_bytes);
	seq_puts(s, "  real_capture_programmed: 0\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_directmemory_drain_path);

static int hd60pro_windows_contiguous_buffer_layout_show(struct seq_file *s,
							 void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u16 command = 0;
	unsigned int i;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "windows_contiguous_buffer_layout: decoded 0x14028d250 init allocation layout; no hardware writes\n");
	seq_puts(s, "source_range: e60MZ0380.X64.SYS 0x14028e167..0x14028e622 allocates, 0x14028e81c..0x14028ebc1 frees\n");
	seq_puts(s, "allocator_imports: 0x1402a51f0=MmAllocateContiguousMemorySpecifyCache-like, 0x1402a5230=MmGetPhysicalAddress-like, 0x1402a51f8=free-contiguous-like\n");
	seq_puts(s, "control_buffer:\n");
	seq_puts(s, "  device+0xe0 size, device+0xd0 virtual, device+0xc8 physical\n");
	seq_puts(s, "  alloc_primary: 0x14028e19c stores VA at +0xd0, 0x14028e1b7 stores PA at +0xc8\n");
	seq_puts(s, "  alloc_retry: 0x14028e20c stores VA at +0xd0, 0x14028e23a stores PA at +0xc8 after 128MiB window retries\n");
	seq_puts(s, "  cleanup: 0x14028e81c or 0x14028ea8d frees +0xd0 using +0xc8/+0xe0 when DMA-adapter mode is active\n");
	seq_puts(s, "status_buffer:\n");
	seq_puts(s, "  device+0x148 size, device+0x140 virtual, device+0x138 physical\n");
	seq_puts(s, "  alloc_primary: 0x14028e2a2 stores VA at +0x140, 0x14028e2bd stores PA at +0x138\n");
	seq_puts(s, "  alloc_retry: 0x14028e3f2 stores VA at +0x140 and rejoins the physical-address store\n");
	seq_puts(s, "  cleanup: 0x14028e8a6 or 0x14028eb18 frees +0x140 using +0x148\n");
	seq_puts(s, "channel_buffers:\n");
	seq_puts(s, "  channel geometry is selected from capture format fields at device+0x28 before allocation; r13 becomes 4, 8, 16, or 32 slots depending on format\n");
	seq_puts(s, "  virtual table: device+0x1190 + slot*8\n");
	seq_puts(s, "  physical table: device+0x190 + slot*0x10\n");
	seq_puts(s, "  size/meta table: device+0x198 + slot*0x10\n");
	seq_puts(s, "  software state table: device+0x2590 + group*0x80 + slot*4\n");
	seq_puts(s, "  cleanup size table: device+0x2990 + group*4\n");
	seq_puts(s, "  alloc_primary: 0x14028e4db VA, 0x14028e510 PA, 0x14028e52f size/meta\n");
	seq_puts(s, "  alloc_retry: 0x14028e5c3 VA, 0x14028e5f5 PA, 0x14028e611 size/meta\n");
	seq_puts(s, "  cleanup: 0x14028e8f3..0x14028e983 and 0x14028eb62..0x14028ebd5 clear VA/PA/meta/state across 8*32 logical slots\n");
	seq_puts(s, "important_negative_result: this constructor prepares host physical addresses but does not itself publish them to BAR0/BAR5 or firmware command records\n");
	seq_puts(s, "linux_status:\n");
	seq_printf(s, "  pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  windows_dma_control_prepared: %d\n",
		   hd->win_dma_control_cpu ? 1 : 0);
	seq_printf(s, "  windows_dma_status_prepared: %d\n",
		   hd->win_dma_status_cpu ? 1 : 0);
	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++) {
		seq_printf(s, "  windows_dma_channel%u_prepared: %d\n",
			   i, hd->win_dma_channel_cpu[i] ? 1 : 0);
	}
	seq_puts(s, "next_static_targets:\n");
	seq_puts(s, "  xrefs that read device+0xc8/+0x138/+0x190 outside 0x14028d250 constructor/destructor\n");
	seq_puts(s, "  callsites using DMA adapter vtable entries from device+0x40 around +0x18/+0x30\n");
	seq_puts(s, "  endpoint event enqueue path that writes the 0x2c-byte /sys/vpl_pciep/epint records\n");
	seq_puts(s, "  any mailbox packet carrying host PA low/high words or sizes after the A2 pipeline-ready state\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_contiguous_buffer_layout);

static int hd60pro_capture_start_plan_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;
	u16 command;
	u32 bar5_040 = hd->bar5 ? ioread32(hd->bar5 + 0x040) : U32_MAX;
	u32 bar5_044 = hd->bar5 ? ioread32(hd->bar5 + 0x044) : U32_MAX;
	u32 bar5_048 = hd->bar5 ? ioread32(hd->bar5 + 0x048) : U32_MAX;
	u32 bar5_04c = hd->bar5 ? ioread32(hd->bar5 + 0x04c) : U32_MAX;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "capture_start_plan: no hardware writes; reverse-engineering checklist for real capture\n");
	seq_puts(s, "windows_config_direct_dma_property: CustomAnalogVideoDirectDMAProperty -> context+0x81e4\n");
	seq_puts(s, "windows_config_direct_memory_mode_property: CustomAnalogVideoDirectMemoryModeProperty -> context+0x8228\n");
	seq_puts(s, "windows_direct_memory_blob: context+0x822c, 16 bytes copied to userspace on direct-memory frame query\n");
	seq_puts(s, "windows_frame_delivery_helper: 0x14027eb38, called by DirectMemory stream path and three other frame-delivery callsites\n");
	seq_puts(s, "windows_likely_stream_object_fields: object+0xb8 buffer length, object+0xc0 back-pointer, object+0x38 active payload/result size\n");
	seq_puts(s, "windows_likely_frame_object_fields: frame+0x10 metadata, frame+0x20 buffer, frame+0x28 payload/stride, frame+0x2c aux, frame+0x35 flag\n");
	seq_puts(s, "windows_stream_start_state_a: 0x140250d57 writes context+0x8144/+0x8148/+0x814c/+0x8578 then context+0x72c0=1\n");
	seq_puts(s, "windows_stream_start_state_b: 0x14026edc6 repeats the same context stream-on state after resolution update\n");
	seq_puts(s, "windows_stream_8144_model: (stream_info+0x30c << 16) | stream_info+0x310, likely width/height pair\n");
	seq_puts(s, "windows_stream_8148_model: stream_info+0x314, local 1080p60 default model stores 60\n");
	seq_puts(s, "windows_stream_814c_model: stream_info+0x31c, interlace/derived height flag model\n");
	seq_puts(s, "windows_stream_8578_model: 0x0000bb80, 48000 decimal; likely audio/sample or timing constant also used near start\n");
	seq_puts(s, "windows_stream_72c0_model: streaming/on flag, 1=start 0=stop\n");
	seq_puts(s, "firmware_endpoint_control: /sys/vpl_pciep/hready /sys/vpl_pciep/epint /sys/class/vpl_pciep/channel_done\n");
	seq_puts(s, "firmware_capture_control: endpoint command 0x29 SET_VIC_PARAMS then command 0x2a/audio_ctrl/epint path, with VideoCap_StartVIC/WaitVIC/GetBufVIC/ReleaseBufVIC inside firmware userland\n");
	seq_puts(s, "firmware_set_vic_payload_model: cmd=0x29 payload_bytes=0x28 off05=fps off06=fw_or_mode off08=width off0a=height off22=interrupt_reduce\n");
	seq_puts(s, "candidate_set_vic_1080p60: cmd=0x29 flags0=0 fps=60 fw_or_mode=7 width=1920 height=1080 interrupt_reduce=0\n");
	seq_puts(s, "candidate_post_set_vic_notify: cmd=0x2a payload_bytes=0x14 likely follows 0x29 and selects epint_1080p when fw_or_mode==7\n");
	seq_puts(s, "linux_step_1_done: mailbox/post-logo/A2 init reaches pipeline_ready\n");
	seq_puts(s, "linux_step_2_done: V4L2/vb2 buffers and DirectMemory-shaped metadata are plumbed\n");
	seq_puts(s, "linux_step_3_done: coherent host descriptor/frame buffers can be allocated\n");
	seq_puts(s, "linux_step_4_missing: endpoint shared-memory advertisement format\n");
	seq_puts(s, "linux_step_5_missing: SET_VIC_PARAMS-equivalent mailbox/endpoint command mapping\n");
	seq_puts(s, "linux_step_6_missing: channel_done/epint interrupt acknowledgement and frame ownership protocol\n");
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "irq_requested: %d\n", hd->irq >= 0);
	seq_printf(s, "pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_printf(s, "dma_buffers_prepared: %d\n",
		   hd->dma_desc_cpu != NULL && hd->dma_frame_cpu != NULL);
	seq_printf(s, "candidate_host_desc_dma: %pad\n", &hd->dma_desc_dma);
	seq_printf(s, "candidate_host_desc_size: %zu\n", hd->dma_desc_size);
	seq_printf(s, "candidate_host_frame_dma: %pad\n", &hd->dma_frame_dma);
	seq_printf(s, "candidate_host_frame_size: %zu\n", hd->dma_frame_size);
	if (desc) {
		seq_printf(s, "candidate_host_desc_magic: 0x%08x\n",
			   le32_to_cpu(desc->magic));
		seq_printf(s, "candidate_host_desc_windows_stream_8144: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8144));
		seq_printf(s, "candidate_host_desc_windows_stream_8148: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8148));
		seq_printf(s, "candidate_host_desc_windows_stream_814c: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_814c));
		seq_printf(s, "candidate_host_desc_windows_stream_8578: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8578));
		seq_printf(s, "candidate_host_desc_windows_stream_72c0: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_72c0));
	}
	seq_printf(s, "bar5_irq_payload0_0x40: 0x%08x\n", bar5_040);
	seq_printf(s, "bar5_irq_payload1_0x44: 0x%08x\n", bar5_044);
	seq_printf(s, "bar5_irq_payload2_0x48: 0x%08x\n", bar5_048);
	seq_printf(s, "bar5_irq_payload3_0x4c: 0x%08x\n", bar5_04c);
	seq_puts(s, "real_capture_programmed: 0\n");
	seq_puts(s, "safe_next_probe: decode Windows writes around the first transition from pipeline_ready to stream-start/hready\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_capture_start_plan);

static int hd60pro_endpoint_command_plan_show(struct seq_file *s, void *unused)
{
	struct hd60pro_ep_set_vic_model vic = {
		.command_id = HD60PRO_EP_CMD_SET_VIC_PARAMS,
		.flags0 = 0,
		.fps = 60,
		.fw_or_mode = 7,
		.width = cpu_to_le16(HD60PRO_DEFAULT_WIDTH),
		.height = cpu_to_le16(HD60PRO_DEFAULT_HEIGHT),
		.interrupt_reduce = 0,
	};

	seq_puts(s, "endpoint_command_plan: no hardware writes; candidate endpoint stream-start model\n");
	seq_puts(s, "source: yuan_demo_sdi/drivers/ep.ko pciep_isr + sysfs attribute strings\n");
	seq_puts(s, "transport_status: unknown; these are firmware-side ep_command/event fields, not proven host mailbox packets\n");
	seq_printf(s, "set_vic_command_id: 0x%02x\n", vic.command_id);
	seq_printf(s, "set_vic_declared_payload_bytes: 0x%02x\n",
		   hd60pro_ep_ints_size[HD60PRO_EP_CMD_SET_VIC_PARAMS]);
	seq_printf(s, "set_vic_candidate_flags0_off04: 0x%02x\n", vic.flags0);
	seq_printf(s, "set_vic_candidate_fps_off05: %u\n", vic.fps);
	seq_printf(s, "set_vic_candidate_fw_or_mode_off06: %u\n",
		   vic.fw_or_mode);
	seq_printf(s, "set_vic_candidate_width_off08: %u\n",
		   le16_to_cpu(vic.width));
	seq_printf(s, "set_vic_candidate_height_off0a: %u\n",
		   le16_to_cpu(vic.height));
	seq_printf(s, "set_vic_candidate_interrupt_reduce_off22: %u\n",
		   vic.interrupt_reduce);
	seq_printf(s, "post_set_vic_command_id: 0x%02x\n",
		   HD60PRO_EP_CMD_POST_SET_VIC);
	seq_printf(s, "post_set_vic_declared_payload_bytes: 0x%02x\n",
		   hd60pro_ep_ints_size[HD60PRO_EP_CMD_POST_SET_VIC]);
	seq_puts(s, "pciep_isr_side_effect_0x29: logs SET_VIC_PARAMS; zero width/height sets no_signal; mode 7 selects epint_1080p path\n");
	seq_puts(s, "pciep_isr_side_effect_0x2a: if no_signal=0, may set interrupt-reduce flag, then notifies audio_ctrl and epint/epint_1080p\n");
	seq_puts(s, "blocking_question: how Windows writes this event into endpoint-visible ep_command memory from host BAR0/BAR5\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_endpoint_command_plan);

static void hd60pro_fill_set_vic_event_record(u8 event[HD60PRO_EP_EVENT_RECORD_BYTES])
{
	memset(event, 0, HD60PRO_EP_EVENT_RECORD_BYTES);
	event[0x00] = HD60PRO_EP_CMD_SET_VIC_PARAMS;
	event[0x04] = 0;	/* channel */
	event[0x05] = 60;	/* fps */
	event[0x06] = 7;	/* firmware mode; 7 routes to epint_1080p */
	event[0x07] = 0;	/* progressive */
	event[0x08] = HD60PRO_DEFAULT_WIDTH & 0xff;
	event[0x09] = HD60PRO_DEFAULT_WIDTH >> 8;
	event[0x0a] = HD60PRO_DEFAULT_HEIGHT & 0xff;
	event[0x0b] = HD60PRO_DEFAULT_HEIGHT >> 8;
	event[0x18] = HD60PRO_DEFAULT_WIDTH & 0xff;
	event[0x19] = HD60PRO_DEFAULT_WIDTH >> 8;
	event[0x1a] = HD60PRO_DEFAULT_HEIGHT & 0xff;
	event[0x1b] = HD60PRO_DEFAULT_HEIGHT >> 8;
	event[0x1c] = 1;	/* bitstream count / channel count model */
}

static int hd60pro_set_vic_event_record_show(struct seq_file *s, void *unused)
{
	u8 event[HD60PRO_EP_EVENT_RECORD_BYTES];
	unsigned int i;

	hd60pro_fill_set_vic_event_record(event);

	seq_puts(s, "set_vic_event_record: candidate 0x2c-byte firmware epint record; no hardware writes\n");
	seq_puts(s, "source: ep.ko pciep_isr + video_capture_mgr SET_VIC dispatch\n");
	seq_puts(s, "record_transport: unknown host-side path; do not inject until Windows transport is mapped\n");
	seq_printf(s, "record_bytes: %u\n", HD60PRO_EP_EVENT_RECORD_BYTES);
	seq_puts(s, "field_00_command: 0x29\n");
	seq_puts(s, "field_04_channel: 0\n");
	seq_puts(s, "field_05_fps: 60\n");
	seq_puts(s, "field_06_fw_or_mode: 7\n");
	seq_puts(s, "field_07_interlace: 0\n");
	seq_puts(s, "field_08_width: 1920\n");
	seq_puts(s, "field_0a_height: 1080\n");
	seq_puts(s, "field_18_input_frame_width_model: 1920\n");
	seq_puts(s, "field_1a_input_frame_height_model: 1080\n");
	seq_puts(s, "field_1c_bitstream_count_model: 1\n");
	seq_puts(s, "hex:");
	for (i = 0; i < HD60PRO_EP_EVENT_RECORD_BYTES; i++) {
		if (!(i % 16))
			seq_printf(s, "\n  %02x:", i);
		seq_printf(s, " %02x", event[i]);
	}
	seq_puts(s, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_set_vic_event_record);

static int hd60pro_endpoint_transport_plan_show(struct seq_file *s,
						void *unused)
{
	struct hd60pro_dev *hd = s->private;
	resource_size_t len = hd60pro_mailbox_len(hd);
	u32 bar5_030 = hd->bar5 ? ioread32(hd->bar5 + 0x030) : U32_MAX;
	u32 bar5_040 = hd->bar5 ? ioread32(hd->bar5 + 0x040) : U32_MAX;
	u32 bar5_044 = hd->bar5 ? ioread32(hd->bar5 + 0x044) : U32_MAX;
	u32 bar5_048 = hd->bar5 ? ioread32(hd->bar5 + 0x048) : U32_MAX;
	u32 bar5_04c = hd->bar5 ? ioread32(hd->bar5 + 0x04c) : U32_MAX;
	u32 bar5_050 = hd->bar5 ? ioread32(hd->bar5 + 0x050) : U32_MAX;
	u32 bar5_0d4 = hd->bar5 ? ioread32(hd->bar5 + 0x0d4) : U32_MAX;

	seq_puts(s, "endpoint_transport_plan: no hardware writes; candidate host-to-firmware transport map\n");
	seq_puts(s, "known_working_transport_family: Windows logo/status upload uses mailbox command 0x60 prepare, host copies payload at BAR0+0x60, then command 0x61 commit\n");
	seq_puts(s, "logo_prepare_packet_shape: [0x800,0x60,selector,0x25800,0x00f00140] with flags=1\n");
	seq_puts(s, "logo_commit_packet_shape: [0x800,0x61,1] with flags=1, Windows sleeps 100ms and checks BAR0+0x08\n");
	seq_puts(s, "logo_selectors_seen: 0x100 no-signal, 0x200 HDCP, 0x300 still\n");
	seq_printf(s, "bar0_payload_window_offset: 0x%02x\n",
		   HD60PRO_FW_WINDOW_OFFSET);
	seq_printf(s, "bar0_selected_mailbox_len: %pa\n", &len);
	seq_printf(s, "logo_payload_size: 0x%05x\n",
		   HD60PRO_LOGO_PAYLOAD_SIZE);
	seq_printf(s, "logo_payload_descriptor: 0x%08x\n",
		   HD60PRO_LOGO_DESCRIPTOR);
	seq_puts(s, "firmware_epint_record_model: video_capture_mgr reads/writes 0x2c-byte records from /sys/vpl_pciep/epint\n");
	seq_puts(s, "candidate_set_vic_record: command 0x29, record size 0x2c, declared SET_VIC payload 0x28\n");
	seq_puts(s, "candidate_post_set_vic_record: command 0x2a, declared payload 0x14, notifies audio_ctrl then epint/epint_1080p\n");
	seq_puts(s, "command_store_limit: firmware /sys/vpl_pciep/command accepts IDs 0x18..0x22 only; 0x29/0x2a are ISR/event IDs, not command_store IDs\n");
	seq_puts(s, "hypothesis_a: mailbox 0x60/0x61 is a generic host payload staging mechanism, and selector chooses firmware-side consumer/type\n");
	seq_puts(s, "hypothesis_b: SET_VIC is not sent through command_store, but through endpoint interrupt/event records produced by a host-side path still hidden in Windows\n");
	seq_puts(s, "hypothesis_c: BAR5 payload registers 0x40..0x4c mirror endpoint outbound/window or pending event state after 0x60/0x61\n");
	seq_puts(s, "unsafe_probe_not_run: do not send a fake 0x29 record through 0x60/0x61 until selector and commit semantics are recovered\n");
	seq_puts(s, "next_static_reverse_targets: callsites around Windows logo 0x60/0x61, firmware event IDs 0x50..0x52/0x60..0x62/0x6e, and any host copy into BAR0+0x60 smaller than 0x25800\n");
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "current_bar5_030_irq_status_or_scratch: 0x%08x\n",
		   bar5_030);
	seq_printf(s, "current_bar5_040_payload0: 0x%08x\n", bar5_040);
	seq_printf(s, "current_bar5_044_payload1: 0x%08x\n", bar5_044);
	seq_printf(s, "current_bar5_048_payload2: 0x%08x\n", bar5_048);
	seq_printf(s, "current_bar5_04c_payload3: 0x%08x\n", bar5_04c);
	seq_printf(s, "current_bar5_050_queue_or_window_state: 0x%08x\n",
		   bar5_050);
	seq_printf(s, "current_bar5_0d4_firmware_or_window_state: 0x%08x\n",
		   bar5_0d4);
	seq_puts(s, "candidate_minimal_future_probe: stage exactly one 0x2c SET_VIC record in BAR0+0x60 only after identifying a non-logo selector or Windows copy size 0x2c/0x28\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_endpoint_transport_plan);

static int hd60pro_windows_payload_uploader_show(struct seq_file *s,
						 void *unused)
{
	struct hd60pro_dev *hd = s->private;
	resource_size_t len = hd60pro_mailbox_len(hd);
	u32 bar5_040 = hd->bar5 ? ioread32(hd->bar5 + 0x040) : U32_MAX;
	u32 bar5_048 = hd->bar5 ? ioread32(hd->bar5 + 0x048) : U32_MAX;

	seq_puts(s, "windows_payload_uploader: decoded Windows staged payload upload path; no hardware writes\n");
	seq_puts(s, "main_helper: e60MZ0380.X64.SYS 0x140277150\n");
	seq_puts(s, "direct_callers: 0x140244e10 is the only direct call in the current objdump\n");
	seq_puts(s, "direct_caller_140244e10: copies 16-byte property/context blobs to context+0x1a8d4/+0x1a8e4/+0x1a8f4, builds a 0x208-byte payload with 0x14027596c, selector from context+0x1c944, then calls 0x140277150\n");
	seq_puts(s, "specialized_siblings: 0x140276624 no-signal logo, 0x140276a28 HDCP logo, 0x140276dbc still logo use the same BAR0+0x60 and 0x60/0x61 staged-upload pattern\n");
	seq_puts(s, "file_loader_helper: 0x1402762d4 opens a resource, copies it to BAR0+0x60, then commits with command 0x0c\n");
	seq_puts(s, "payload_validation_140277150: reads a blob, requires magic 0x55aa55aa, width <= 0x140, height <= 0x0f0, size > 0x0c\n");
	seq_puts(s, "payload_copy: rep movsd to context+0x108+0x60; context+0x108 maps Linux BAR0\n");
	seq_puts(s, "prepare_packet_shape: [0x800,0x60,selector << 16,size,(height << 16) | width]\n");
	seq_puts(s, "commit_packet_shape: [0x800,0x61,1]\n");
	seq_puts(s, "commit_check: Windows sleeps about 100ms then expects BAR0+0x08 == 0\n");
	seq_puts(s, "observed_logo_prepare: selector 1/2/3 become 0x00010000/0x00020000/0x00030000, size 0x25800, descriptor 0x00f00140\n");
	seq_puts(s, "important_negative_result: this uploader is image/blob-oriented and does not match the 0x2c SET_VIC endpoint record directly\n");
	seq_puts(s, "capture_relevance: command 0x60/0x61 is still a proven host-to-firmware staging primitive, but decoded callers remain blob/logo/property upload paths rather than stream-start DMA setup\n");
	seq_puts(s, "unsafe_probe_not_run: no fake selector or SET_VIC injection is attempted from this node\n");
	seq_printf(s, "bar0_selected_mailbox_len: %pa\n", &len);
	seq_printf(s, "payload_window_offset: 0x%02x\n",
		   HD60PRO_FW_WINDOW_OFFSET);
	seq_printf(s, "current_bar5_payload0_0x40: 0x%08x\n", bar5_040);
	seq_printf(s, "current_bar5_payload2_0x48: 0x%08x\n", bar5_048);
	seq_puts(s, "next_static_reverse_targets: cross-reference 0x140277150 callers beyond logo, and search for a non-image payload uploader or direct epint shared-memory write path\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_payload_uploader);

static int hd60pro_windows_stream_state_flow_show(struct seq_file *s,
						  void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;
	u16 command = 0;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "windows_stream_state_flow: decoded Windows stream-state/control flow; no hardware writes\n");
	seq_puts(s, "primary_stream_start_path: 0x140250cfe..0x140250d57 writes context stream state and sets context+0x72c0=1\n");
	seq_puts(s, "resolution_worker_path: 0x14026dec0 iterates sibling/channel contexts, attaches context+0x1f1a8/+0x1f1b0, updates 0x88 signal/video registers, then repeats stream-state write at 0x14026ed84..0x14026edc6\n");
	seq_puts(s, "format_property_path: 0x140283462..0x14028358a fills stream_info+0x30c/+0x310/+0x314/+0x31c/+0x33c/+0x340/+0x344/+0x34c then writes context+0x8144/+0x8148/+0x814c/+0x81e0/+0x8578/+0x72c0\n");
	seq_puts(s, "mode_change_gate_140283602: if stream_info+0x2080 changed, call 0x14026dec0; otherwise call bridge attach 0x140254614\n");
	seq_puts(s, "stream_info_2080_modes_seen: 0x2400, 0x2601, 0x2611, 0x260c, 0x270c\n");
	seq_puts(s, "channel_count_model: 0x2400/0x2611/0x270c use up to four channels; other decoded modes commonly use two\n");
	seq_puts(s, "state_fields:\n");
	seq_puts(s, "  context+0x727c = frame/rate class; Windows stores 1 for fps >= 0x33 or near 0x1a..0x1e, otherwise 0x10\n");
	seq_puts(s, "  context+0x8144 = (stream_info+0x30c << 16) | stream_info+0x310, likely width/height pair\n");
	seq_puts(s, "  context+0x8148 = stream_info+0x314, fps with 59/61 normalized to 60 in one path\n");
	seq_puts(s, "  context+0x814c = stream_info+0x31c, interlace/height-family flag\n");
	seq_puts(s, "  context+0x8578 = 0x0000bb80, 48000 decimal timing/audio constant\n");
	seq_puts(s, "  context+0x72c0 = streaming flag; 1=start, 0=stop/reset\n");
	seq_puts(s, "observed_1080p60_model: width=1920 height=1080 fps=60 progressive, desc 0x8144=0x07800438 0x8148=0x3c 0x814c=0 0x8578=0xbb80 0x72c0=1\n");
	seq_puts(s, "important_negative_result: these writes are Windows host context state and chip 0x88 signal conditioning, not the final PCI DMA ring programming by themselves\n");
	seq_puts(s, "linux_current_model:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_puts(s, "  real_dma_programmed: 0\n");
	if (desc) {
		seq_printf(s, "  host_desc_windows_stream_8144: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8144));
		seq_printf(s, "  host_desc_windows_stream_8148: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8148));
		seq_printf(s, "  host_desc_windows_stream_814c: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_814c));
		seq_printf(s, "  host_desc_windows_stream_8578: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8578));
		seq_printf(s, "  host_desc_windows_stream_72c0: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_72c0));
	}
	seq_puts(s, "next_static_reverse_targets: xrefs to context+0x72c0/context+0x8144 consumers and the first MMIO/register writes that consume this state to advertise host buffers or arm capture DMA\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_stream_state_flow);

static int hd60pro_windows_stream_consumers_show(struct seq_file *s,
						 void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;

	seq_puts(s, "windows_stream_consumers: decoded consumers of Windows stream-state fields; no hardware writes\n");
	seq_puts(s, "field_group: context+0x72c0/+0x727c/+0x8144/+0x8148/+0x814c/+0x8578/+0x81e0\n");
	seq_puts(s, "property_accessors_14023eb70_family: simple KS-style getters/setters for +0x8144, +0x8148, +0x814c and derived format values\n");
	seq_puts(s, "streaming_getter_14022e540: query case reads context+0x72c0 into userspace/status buffer alongside +0x73d0\n");
	seq_puts(s, "format_builders_140218c98_and_14021a473: read +0x8144/+0x8148/+0x814c/+0x727c, derive width/height/fps/interlace, then call 0x14021ee5c and 0x140228918\n");
	seq_puts(s, "direct_memory_frame_path_14027e7b0_14027f073: reads +0x8148/+0x814c/+0x727c to compute frame periods and metadata timestamps\n");
	seq_puts(s, "direct_memory_streaming_gates: 0x14027da69, 0x14027db98, 0x14027ef1b, 0x14027f1d8 and siblings compare +0x72c0 before updating frame/drop counters\n");
	seq_puts(s, "frame_delivery_helper_14027eb38: called from DirectMemory path and other frame-delivery callsites; uses stream state to synthesize timing/metadata when no real frame is ready\n");
	seq_puts(s, "reset_paths: 0x140250e82, 0x140253d5f, 0x14026e14c, 0x1402835d8 clear stream-state fields and +0x72c0\n");
	seq_puts(s, "important_negative_result: these consumers explain V4L2/DirectMemory metadata and timing, but still do not expose the PCI endpoint buffer advertisement or DMA ring registers\n");
	seq_puts(s, "linux_current_values:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  v4l2_synthetic_frames: %d\n", synthetic_v4l2);
	seq_printf(s, "  v4l2_frames_completed: %u\n", hd->sequence);
	if (desc) {
		seq_printf(s, "  0x8144_model: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8144));
		seq_printf(s, "  0x8148_model: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8148));
		seq_printf(s, "  0x814c_model: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_814c));
		seq_printf(s, "  0x8578_model: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8578));
		seq_printf(s, "  0x72c0_model: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_72c0));
	}
	seq_puts(s, "next_static_reverse_targets: xrefs from 0x14021ee5c/0x140228918 and non-metadata users of +0x81e4 DirectDMA or endpoint counters +0x204c/+0x2050/+0x2058/+0x205c\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_stream_consumers);

static int hd60pro_windows_frame_counter_info_show(struct seq_file *s,
						   void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "windows_frame_counter_info: decoded DirectMemory frame counter/gate fields; no hardware writes\n");
	seq_puts(s, "field_group: stream_info+0x204c/+0x2050/+0x2054/+0x2058/+0x205c\n");
	seq_puts(s, "set_2050_140253d59: during format/reset path, stores fps/2 into stream_info+0x2050 when context+0x8208 >= 2\n");
	seq_puts(s, "set_2050_140253f7c: stream update path repeats fps/2 store into +0x2050 after chip/pipeline updates\n");
	seq_puts(s, "reset_2054_205c_14027b374: if not streaming or +0x2050 is zero, clears external buffer pointers and +0x2054/+0x205c\n");
	seq_puts(s, "gate_205c_14027db98: DirectMemory timing path clears +0x205c when streaming state or frame counter conditions require reset\n");
	seq_puts(s, "gate_2058_14027ef02: alternate DirectMemory timing path clears +0x2058 under similar stream/frame conditions\n");
	seq_puts(s, "decrement_2050_14027f1f7: DirectMemory frame delivery decrements +0x2050 as a software cadence/drop gate\n");
	seq_puts(s, "zero_140289447: teardown clears qword families at +0x2050 and +0x2058\n");
	seq_puts(s, "important_negative_result: these fields are Windows stream_info software counters, not PCI BAR registers and not proven firmware channel_done ownership words\n");
	seq_puts(s, "linux_model: synthetic V4L2 maintains equivalent frame sequence/timestamp metadata locally, but no real endpoint counter is consumed yet\n");
	seq_printf(s, "linux_pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "linux_v4l2_streaming_now: %d\n", hd->streaming);
	seq_printf(s, "linux_v4l2_frames_completed: %u\n", hd->sequence);
	seq_printf(s, "linux_last_frame_sequence: %u\n",
		   hd->last_frame_meta.sequence);
	seq_printf(s, "linux_last_frame_payload_bytes: %u\n",
		   hd->last_frame_meta.payload_bytes);
	seq_puts(s, "next_static_reverse_targets: exclude +0x2050/+0x2058/+0x205c as primary DMA arm path; continue with physical-buffer consumers and firmware endpoint event transport\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_frame_counter_info);

static int hd60pro_windows_buffer_queue_info_show(struct seq_file *s,
						  void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;

	seq_puts(s, "windows_buffer_queue_info: decoded Windows software frame-buffer queue; no hardware writes\n");
	seq_puts(s, "source_create: 0x14021e190, called from format builders after 0x14021ee5c\n");
	seq_puts(s, "source_destroy: 0x14021e480 frees the same object and compacts context+0x6d58..0x6d90\n");
	seq_puts(s, "context_queue_slots: context+0x6d58/+0x6d60/+0x6d68/+0x6d70/+0x6d78/+0x6d80/+0x6d88/+0x6d90\n");
	seq_puts(s, "create_gate: if all eight queue slots are already populated, create returns STATUS_INSUFFICIENT_RESOURCES\n");
	seq_puts(s, "queue_object_size: 0x140 bytes, pool tag 0x48504a59 ('YJPH' little-endian)\n");
	seq_puts(s, "queue_object_links: +0x128 ks/filter object, +0x130 stream/format descriptor, +0x138 owning context\n");
	seq_puts(s, "queue_object_copy: +0xc0/+0xc8/+0xd0 mirror +0x128/+0x130/+0x138\n");
	seq_puts(s, "queue_object_mode: +0x100 copies stream format field [format+0x08]\n");
	seq_puts(s, "queue_object_allocations: +0xf0 and +0xf8 are two 0xc00-byte buffers; +0x108 is format-size-dependent video buffer\n");
	seq_puts(s, "queue_object_defaults: +0x48=0, +0x50=0, +0x58=3, +0x60=0, +0x64=0, +0xb8=1\n");
	seq_puts(s, "video_buffer_size_table: accepts 0x4380, 0x4920, 0x5a00, 0x5fa0, 0x6540 from format+0x08 before allocating +0x108\n");
	seq_puts(s, "not_dma_yet: allocations are Windows host virtual/pool buffers, not PCI DMA addresses or endpoint outbound registers\n");
	seq_puts(s, "direct_memory_link: frame path 0x14027e0db..0x14027e399 reads queue +0xd0 as a frame base and uses +0xf0/+0xf8/+0x108 metadata buffers\n");
	seq_puts(s, "frame_copy_paths: depending on format/state, Windows copies from queue +0xd0, +0xd0+0x4000/0x8000/0xc000, or external stream buffers at +0x2c8/+0x2d4\n");
	seq_puts(s, "pixel_copy_helper_14022db40: converts/copies between already-populated host buffers; called from frame delivery, not a PCI producer\n");
	seq_puts(s, "external_buffer_path_14027e311: reads stream object +0x2c8 base and +0x2d4 size, then copies with 0x140297500\n");
	seq_puts(s, "real_dma_gap: this queue explains local frame delivery storage, but the PCI endpoint still needs the producer side that fills queue +0xd0 or external buffers\n");
	seq_puts(s, "linux_current_values:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  synthetic_v4l2: %d\n", synthetic_v4l2);
	seq_printf(s, "  dma_buffers_prepared: %d\n", !!desc);
	if (desc) {
		seq_printf(s, "  linux_desc_dma: %pad\n", &hd->dma_desc_dma);
		seq_printf(s, "  linux_frame_dma: %pad\n", &hd->dma_frame_dma);
		seq_printf(s, "  linux_frame_size: %zu\n", hd->dma_frame_size);
		seq_printf(s, "  modeled_stream_8144: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8144));
		seq_printf(s, "  modeled_stream_8148: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_8148));
		seq_printf(s, "  modeled_stream_72c0: 0x%08x\n",
			   le32_to_cpu(desc->windows_stream_72c0));
	}
	seq_puts(s, "next_static_reverse_targets: producer xrefs to context queue slots +0x6d58..+0x6d90, stream object +0x2c8/+0x2d4, and writes to BAR0/BAR5 endpoint/outbound registers before frame-delivery reads these buffers\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_buffer_queue_info);

static int hd60pro_windows_frame_producer_search_show(struct seq_file *s,
						      void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;

	seq_puts(s, "windows_frame_producer_search: decoded queue/external-buffer producer search; no hardware writes\n");
	seq_puts(s, "queue_table_init_140230000: zeroes context+0x6b58/+0x6a98/+0x6cd8/+0x6d58/+0x6dd8/+0x6e58 families\n");
	seq_puts(s, "queue_table_init_result: initialization/reset only; not the frame producer\n");
	seq_puts(s, "queue_create_14021e190: allocates 0x140-byte host queue objects and inserts them into context+0x6d58..+0x6d90\n");
	seq_puts(s, "queue_destroy_14021e480: frees/compacts context+0x6d58..+0x6d90; confirms these are host queue slots\n");
	seq_puts(s, "external_buffer_consumer_14027e311: reads stream object +0x2c8 as source base and +0x2d4 as source size\n");
	seq_puts(s, "external_buffer_copy_14027e34f: copies from the +0x2c8 buffer with helper 0x140297500 into the delivered frame buffer\n");
	seq_puts(s, "directmemory_meta_14027e3b0: updates timestamp/duration/payload metadata using stream_info+0x58/+0x40/+0x48 and frame counters\n");
	seq_puts(s, "directmemory_counter_fields: stream_info+0x9c/+0xdc/+0x15c/+0x19c and context+0x97f0/+0x98c0/+0x98c4 participate in timing/drop decisions\n");
	seq_puts(s, "observed_frame_base_consumers: queue+0xd0, queue+0xd0+0x4000/+0x8000/+0xc000, and stream object +0x2c8/+0x2d4 are consumers of already-visible memory\n");
	seq_puts(s, "important_negative_result: decoded paths explain software buffering, copy, and DirectMemory metadata; they still do not program endpoint outbound windows or PCI DMA\n");
	seq_puts(s, "candidate_producer_fields_to_trace_next: writes to stream object +0x2c8/+0x2d4, queue +0xd0, context+0x81e4 DirectDMA, and endpoint counters +0x204c/+0x2050/+0x2058/+0x205c\n");
	seq_puts(s, "candidate_hardware_side_to_trace_next: first Windows call that passes host physical addresses or BAR0+0x60 payloads after A2 pipeline-ready and before channel_done/epint activity\n");
	seq_puts(s, "linux_current_values:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  dma_buffers_prepared: %d\n", !!desc);
	if (desc) {
		seq_printf(s, "  linux_desc_dma: %pad\n", &hd->dma_desc_dma);
		seq_printf(s, "  linux_frame_dma: %pad\n", &hd->dma_frame_dma);
		seq_printf(s, "  linux_frame_size: %zu\n", hd->dma_frame_size);
		seq_printf(s, "  descriptor_magic: 0x%08x\n",
			   le32_to_cpu(desc->magic));
		seq_printf(s, "  descriptor_frame_bytes: 0x%08x\n",
			   le32_to_cpu(desc->frame_bytes));
	}
	seq_puts(s, "next_driver_step: keep /dev/video0 synthetic but add a guarded endpoint event/DMA programming node only after the producer write path is identified\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_frame_producer_search);

static int hd60pro_windows_external_buffer_info_show(struct seq_file *s,
						     void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;

	seq_puts(s, "windows_external_buffer_info: decoded stream object +0x2c8/+0x2d4 lifecycle; no hardware writes\n");
	seq_puts(s, "live_object_alloc_14027a2d6: allocates five 0x00655000 buffers at stream+0x1f8/+0x200/+0x208/+0x210 and five 0x001d4c00 buffers at +0x2a8/+0x2b0/+0x2b8/+0x2c0/+0x2c8\n");
	seq_puts(s, "live_object_alloc_14027a372: stores final 0x001d4c00 allocation pointer into stream object +0x2c8\n");
	seq_puts(s, "live_object_free_1402824e1: frees stream object +0x2c8 and clears it to zero with the same cleanup sweep as +0x2a8..+0x2c0\n");
	seq_puts(s, "live_object_clear_14028ac57: clears stream object +0x2c8 for 0x001d4c00 bytes using helper 0x1402977c0 before recomputing +0x2d4\n");
	seq_puts(s, "live_object_size_14028acef: computes stream object +0x2d4 from stream_info timing/rate fields; 48kHz model uses 0x000bb800 or 0x0002ee00 divided by derived cadence\n");
	seq_puts(s, "template_writes_14021f843_to_14022bb24: many rbp+0x2c8/+0x2d4 writes are stack/local format templates, commonly with +0x2d4=0x00100000, not the live stream object\n");
	seq_puts(s, "consumer_14027e311: copies from live stream +0x2c8/+0x2d4 into delivered frame memory after the producer has already populated that buffer\n");
	seq_puts(s, "important_negative_result: +0x2c8 is now identified as Windows host memory allocation, so the missing hardware path is the earlier firmware/endpoint producer that fills this host memory\n");
	seq_puts(s, "remaining_hardware_targets: Windows calls that advertise these allocation addresses to the firmware endpoint, pcie_set_outbound-equivalent BAR5 writes, or BAR0+0x60 payloads carrying host addresses\n");
	seq_puts(s, "linux_current_values:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  dma_buffers_prepared: %d\n", !!desc);
	if (desc) {
		seq_printf(s, "  linux_candidate_frame_dma: %pad\n",
			   &hd->dma_frame_dma);
		seq_printf(s, "  linux_candidate_frame_size: %zu\n",
			   hd->dma_frame_size);
		seq_printf(s, "  windows_live_external_alloc_size: 0x%08x\n",
			   0x001d4c00);
		seq_printf(s, "  windows_template_external_size: 0x%08x\n",
			   0x00100000);
	}
	seq_puts(s, "next_driver_step: do not map linux_frame_dma into hardware until the Windows address-advertisement call is recovered\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_external_buffer_info);

static int hd60pro_windows_dma_mapping_info_show(struct seq_file *s,
						 void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct hd60pro_host_frame_desc *desc = hd->dma_desc_cpu;
	resource_size_t bar0_start = pci_resource_start(hd->pdev, HD60PRO_BAR0);
	resource_size_t bar5_start = pci_resource_start(hd->pdev, HD60PRO_BAR5);
	u16 command;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "windows_dma_mapping_info: decoded Windows PCI/resource/DMA init candidates; no hardware writes\n");
	seq_puts(s, "import_0x1402a5068: KsDeviceGetBusData\n");
	seq_puts(s, "import_0x1402a5060: KsDeviceRegisterAdapterObject\n");
	seq_puts(s, "import_0x1402a5190: MmMapIoSpace\n");
	seq_puts(s, "import_0x1402a51f0: MmAllocateContiguousMemorySpecifyCache\n");
	seq_puts(s, "import_0x1402a51f8: MmFreeContiguousMemorySpecifyCache\n");
	seq_puts(s, "import_0x1402a5210: IoGetDmaAdapter\n");
	seq_puts(s, "import_0x1402a5230: MmGetPhysicalAddress\n");
	seq_puts(s, "cfg_dispatch_0x1402a52d0: local guard-dispatch pointer to 0x1402974c0 (jmp rax); not an import and not a DMA helper\n");
	seq_puts(s, "resource_map_14028d8d1: maps translated resources with MmMapIoSpace into device slots; slot0 maps device+0x108 and matches BAR0 model, slot1 likely maps BAR5 on this card\n");
	seq_puts(s, "pci_config_reads_14028d553: reads bus data around PCI config offsets derived from resource index, then stores board/class bytes into device+0x28 descriptor\n");
	seq_puts(s, "contig_page_14028d628: allocates one 0x1000-byte contiguous buffer, records its physical address, then immediately frees it; appears to be DMA-address capability/probe logging, not a persistent frame ring\n");
	seq_puts(s, "dma_adapter_14028d83d: calls IoGetDmaAdapter and stores adapter pointer at device+0x40 plus map-register count at device+0x48\n");
	seq_puts(s, "ks_adapter_register_14028e2ee: calls KsDeviceRegisterAdapterObject with device+0x40, value 0xfffffff8, and alignment/count 0x10\n");
	seq_puts(s, "persistent_contig_buffers_14028e19c: allocates device+0xd0 with physical address at device+0xc8 when device+0xe0 size is nonzero\n");
	seq_puts(s, "persistent_contig_buffers_14028e2a2: allocates device+0x140 with physical address at device+0x138 when device+0x148 size is nonzero\n");
	seq_puts(s, "per_channel_contig_14028e4db: allocates per-channel buffers at device+0x1190-family and stores physical addresses in device+0x190-family\n");
	seq_puts(s, "per_channel_contig_fallback_14028e5c3: fallback allocation path uses a large upper-bound address window when first per-channel allocation fails\n");
	seq_puts(s, "important_result: this is the first confirmed Windows path that creates persistent physical-address-backed buffers; it is a stronger DMA target than stream object +0x2c8\n");
	seq_puts(s, "rejected_hypothesis: calls through 0x1402a52d0 are generic CFG indirect calls, so they are not evidence of a DMA adapter callback by themselves\n");
	seq_puts(s, "still_missing: call path that writes device+0xc8/+0x138/+0x190 physical addresses to BAR0/BAR5 or to a firmware endpoint payload\n");
	seq_puts(s, "linux_current_values:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_printf(s, "  dma_buffers_prepared: %d\n", !!desc);
	seq_printf(s, "  linux_bar0_start: 0x%016llx\n",
		   (unsigned long long)bar0_start);
	seq_printf(s, "  linux_bar0_len: 0x%016llx\n",
		   (unsigned long long)hd->bar0_len);
	seq_printf(s, "  linux_bar5_start: 0x%016llx\n",
		   (unsigned long long)bar5_start);
	seq_printf(s, "  linux_bar5_len: 0x%016llx\n",
		   (unsigned long long)hd->bar5_len);
	if (desc) {
		seq_printf(s, "  linux_desc_dma: %pad\n", &hd->dma_desc_dma);
		seq_printf(s, "  linux_desc_size: %zu\n", hd->dma_desc_size);
		seq_printf(s, "  linux_frame_dma: %pad\n", &hd->dma_frame_dma);
		seq_printf(s, "  linux_frame_size: %zu\n", hd->dma_frame_size);
	}
	seq_puts(s, "next_static_reverse_targets: xrefs to device+0xc8/+0xd0/+0x138/+0x140/+0x190/+0x1190 after 0x14028d83d and any mailbox/BAR write that carries those physical addresses\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_dma_mapping_info);

static int hd60pro_windows_dma_publish_search_show(struct seq_file *s,
						   void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u16 command = 0;
	unsigned int i;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "windows_dma_publish_search: current search for the host-physical-address advertisement path; no hardware writes\n");
	seq_puts(s, "confirmed_device_context: r14 in 0x14028d250..0x14028e9ee is the PCI device context that owns BAR mappings, DMA adapter, IRQ, and persistent contiguous buffers\n");
	seq_puts(s, "confirmed_imports: 0x1402a51f0=MmAllocateContiguousMemorySpecifyCache, 0x1402a51f8=MmFreeContiguousMemorySpecifyCache, 0x1402a5230=MmGetPhysicalAddress\n");
	seq_puts(s, "device_physical_fields:\n");
	seq_puts(s, "  device+0xc8 physical for control/software-source buffer +0xd0 size +0xe0\n");
	seq_puts(s, "  device+0x138 physical for status buffer +0x140 size +0x148\n");
	seq_puts(s, "  device+0x190 + slot*0x10 physical for per-channel buffer +0x1190 + slot*8, size/meta +0x198 + slot*0x10\n");
	seq_puts(s, "xref_result_device_pa_fields: literal-context search currently finds device+0xc8/+0x138/+0x190 writes in constructor and reads in destructor/free paths, not a publish call\n");
	seq_puts(s, "false_positive_stream_object_14021e190:\n");
	seq_puts(s, "  allocates a 0x140-byte host queue/config object and inserts it into device+0x6d58..+0x6d90\n");
	seq_puts(s, "  object+0xc0/+0xc8/+0xd0 copy host object pointers at 0x14021e2f1..0x14021e314; these are not device physical-address fields\n");
	seq_puts(s, "  object+0xd0 is later copied out by 0x14021d32c and read by DirectMemory frame paths; that is a host memory pointer, not BAR/DMA publication\n");
	seq_puts(s, "false_positive_directmemory_14027d043:\n");
	seq_puts(s, "  reads rbx+0xd0 and offsets +0x2000/+0x4000/+0x6000/+0x8000/+0xc000 as already-visible software source memory\n");
	seq_puts(s, "  copies data into delivered frame buffers through 0x140297500; no physical address is handed to the PCI endpoint there\n");
	seq_puts(s, "most_likely_remaining_windows_publish_sites:\n");
	seq_puts(s, "  code that consumes DMA adapter object device+0x40 outside cleanup callbacks\n");
	seq_puts(s, "  endpoint event enqueue path before firmware /sys/vpl_pciep/epint receives event 0x29/0x2a/channel_done\n");
	seq_puts(s, "  BAR5 outbound-window programming mirror of firmware pcie_set_outbound, especially BAR5+0x50/+0x54/+0x58/+0x74/+0x7c/+0xd4\n");
	seq_puts(s, "  mailbox packets after A2 completion whose payload words look like PA low/high plus size\n");
	seq_puts(s, "linux_current_state:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_printf(s, "  dma_desc_prepared: %d\n", hd->dma_desc_cpu ? 1 : 0);
	seq_printf(s, "  dma_frame_prepared: %d\n", hd->dma_frame_cpu ? 1 : 0);
	seq_printf(s, "  windows_dma_control_prepared: %d\n",
		   hd->win_dma_control_cpu ? 1 : 0);
	seq_printf(s, "  windows_dma_status_prepared: %d\n",
		   hd->win_dma_status_cpu ? 1 : 0);
	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++) {
		seq_printf(s, "  windows_dma_channel%u_prepared: %d\n",
			   i, hd->win_dma_channel_cpu[i] ? 1 : 0);
	}
	seq_puts(s, "driver_policy: keep real_dma_programmed=0 until one publish path is proven by Windows code or a safe observable firmware ack\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_dma_publish_search);

static int hd60pro_windows_bar_mapping_xrefs_show(struct seq_file *s,
						  void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 bar0_30 = hd->bar0 && hd->bar0_len >= 0x34 ?
		      readl(hd->bar0 + 0x30) : 0xffffffff;
	u32 bar0_40 = hd->bar0 && hd->bar0_len >= 0x44 ?
		      readl(hd->bar0 + 0x40) : 0xffffffff;
	u32 bar0_44 = hd->bar0 && hd->bar0_len >= 0x48 ?
		      readl(hd->bar0 + 0x44) : 0xffffffff;
	u32 bar0_48 = hd->bar0 && hd->bar0_len >= 0x4c ?
		      readl(hd->bar0 + 0x48) : 0xffffffff;
	u32 bar0_4c = hd->bar0 && hd->bar0_len >= 0x50 ?
		      readl(hd->bar0 + 0x4c) : 0xffffffff;
	u32 bar0_50 = hd->bar0 && hd->bar0_len >= 0x54 ?
		      readl(hd->bar0 + 0x50) : 0xffffffff;
	u32 bar0_54 = hd->bar0 && hd->bar0_len >= 0x58 ?
		      readl(hd->bar0 + 0x54) : 0xffffffff;
	u32 bar0_58 = hd->bar0 && hd->bar0_len >= 0x5c ?
		      readl(hd->bar0 + 0x58) : 0xffffffff;
	u32 bar0_5c = hd->bar0 && hd->bar0_len >= 0x60 ?
		      readl(hd->bar0 + 0x5c) : 0xffffffff;
	u32 bar5_30 = hd->bar5 && hd->bar5_len >= 0x34 ?
		      readl(hd->bar5 + 0x30) : 0xffffffff;
	u32 bar5_38 = hd->bar5 && hd->bar5_len >= 0x3c ?
		      readl(hd->bar5 + 0x38) : 0xffffffff;
	u32 bar5_dc = hd->bar5 && hd->bar5_len >= 0xe0 ?
		      readl(hd->bar5 + 0xdc) : 0xffffffff;

	seq_puts(s, "windows_bar_mapping_xrefs: decoded Windows device+0x108/+0x110 BAR users; no hardware writes\n");
	seq_puts(s, "resource_mapping_model: 0x14028d8d1 maps translated resources into device context slots\n");
	seq_puts(s, "device_0x108_model: first mapped memory resource, matches Linux BAR0 payload/mailbox aperture at 0xfa000000 length 0x02000000\n");
	seq_puts(s, "device_0x110_model: second mapped memory resource, matches Linux BAR5 sideband aperture at 0xfc000000 length 0x1000\n");
	seq_puts(s, "preinit_pattern_140278c0c: BAR5+0xdc=2, BAR0+0x30=0, BAR0+0x00=0x400 before async mailbox command 0x01 wait loop\n");
	seq_puts(s, "preinit_pattern_1402843b8: same BAR5+0xdc/BAR0+0x30/BAR0+0x00 sequence inside worker/stream state path; then reads BAR0+0x40..0x4c interrupt payload bytes\n");
	seq_puts(s, "payload_publish_140278c8e: writes BAR5+0x30 from device+0x80+4 and BAR5+0x38 from device+0xa0 while preparing payload window references\n");
	seq_puts(s, "irq_payload_consumer_14028444a: reads BAR0+0x40/+0x44/+0x48/+0x4c and feeds event/queue helper 0x1402a51b0; this matches Linux BAR5 payload mirror observations\n");
	seq_puts(s, "mailbox_helper_family: 0x14027785b/0x140277959/0x140277a91/0x140277cec/0x140285074/0x1402851cc operate through device+0x108 mailbox registers\n");
	seq_puts(s, "mailbox_clear_140288303: clears BAR0+0x50/+0x54/+0x58/+0x5c after stream flag gate 0x2044==0xf1 before software table processing\n");
	seq_puts(s, "offset_collision_note: earlier 0x14021d8ac writes to object+0x108/+0x110 are queue/format-object fields, not device BAR slots\n");
	seq_puts(s, "important_negative_result: these xrefs confirm BAR roles and interrupt/payload staging, but still do not show physical frame buffer addresses being advertised to the endpoint\n");
	seq_puts(s, "linux_live_regs:\n");
	seq_printf(s, "  bar0_030_irq_status_or_scratch: 0x%08x\n", bar0_30);
	seq_printf(s, "  bar0_040_irq_payload0: 0x%08x\n", bar0_40);
	seq_printf(s, "  bar0_044_irq_payload1: 0x%08x\n", bar0_44);
	seq_printf(s, "  bar0_048_irq_payload2: 0x%08x\n", bar0_48);
	seq_printf(s, "  bar0_04c_irq_payload3: 0x%08x\n", bar0_4c);
	seq_printf(s, "  bar0_050_clear_family0: 0x%08x\n", bar0_50);
	seq_printf(s, "  bar0_054_clear_family1: 0x%08x\n", bar0_54);
	seq_printf(s, "  bar0_058_clear_family2: 0x%08x\n", bar0_58);
	seq_printf(s, "  bar0_05c_clear_family3: 0x%08x\n", bar0_5c);
	seq_printf(s, "  bar5_030_payload_window_low: 0x%08x\n", bar5_30);
	seq_printf(s, "  bar5_038_payload_window_high_or_end: 0x%08x\n", bar5_38);
	seq_printf(s, "  bar5_0dc_irq_ack_sideband_windows: 0x%08x\n", bar5_dc);
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_puts(s, "next_static_reverse_targets: inspect 0x140284380 worker event path and 0x140288303 stream gate for the first caller that links BAR0 payload records to the persistent physical buffers\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_bar_mapping_xrefs);

static int hd60pro_windows_worker_event_path_show(struct seq_file *s,
						  void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 bar0_40 = hd->bar0 && hd->bar0_len >= 0x44 ?
		      readl(hd->bar0 + 0x40) : 0xffffffff;
	u32 bar0_44 = hd->bar0 && hd->bar0_len >= 0x48 ?
		      readl(hd->bar0 + 0x44) : 0xffffffff;
	u32 bar0_48 = hd->bar0 && hd->bar0_len >= 0x4c ?
		      readl(hd->bar0 + 0x48) : 0xffffffff;
	u32 bar0_4c = hd->bar0 && hd->bar0_len >= 0x50 ?
		      readl(hd->bar0 + 0x4c) : 0xffffffff;

	seq_puts(s, "windows_worker_event_path: decoded worker/event path around 0x14028adf0 and 0x140284380; no hardware writes\n");
	seq_puts(s, "worker_create_14028adf0: if device+0x7688 is empty, stores callback 0x140284380 and creates/starts a Windows worker/thread object\n");
	seq_puts(s, "worker_stop_14028ae74: marks device+0x7699=1, stops the worker object at device+0x7690, then clears it\n");
	seq_puts(s, "worker_loop_gate_140284523: loops until device+0x7698 becomes nonzero\n");
	seq_puts(s, "event_source_1402843b8: reads BAR0+0x30, acks with BAR5+0xdc=2/BAR0+0x30=0/BAR0+0=0x400, then consumes BAR0+0x40..0x4c payload bytes\n");
	seq_puts(s, "event_queue_14028444a: combines BAR0+0x40/+0x44/+0x48 into a 24-bit token and queues it into device+0x29b0 + slot*0x40 using guarded helper at 0x1402a51b0\n");
	seq_puts(s, "event_queue_high_1402844c5: when BAR0+0x30 has high payload bits, reads BAR0+0x4c and queues slots 0x80..0xff with the same helper\n");
	seq_puts(s, "sibling_event_path_14028eccd: another IRQ/event drain reads BAR0+0x40-family, updates global dispatch table 0x1403ac660, clears BAR0+0x50, and queues bitfields into device+0x29b0 slots\n");
	seq_puts(s, "timer_like_sibling_14028294b: uses the same device+0x69b8 lock and 0x1402a51b0 queue helper for software timing/event tokens, confirming helper is queue insertion rather than DMA programming\n");
	seq_puts(s, "cfg_note: 0x1402a51b0/+0x51b8/+0x51c0 are indirect WDF/RTL-style helper imports or dispatch slots; their callsites here operate on host queue structures\n");
	seq_puts(s, "important_result: BAR0+0x40..0x4c is a Windows-visible endpoint event payload path and is probably related to firmware pciep_isr/store_channel_done payload mirrors\n");
	seq_puts(s, "important_negative_result: this path consumes endpoint events after they happen; it still does not show how Windows advertises host frame physical buffers or starts endpoint capture DMA\n");
	seq_puts(s, "linux_live_event_payload:\n");
	seq_printf(s, "  bar0_040_payload0: 0x%08x\n", bar0_40);
	seq_printf(s, "  bar0_044_payload1: 0x%08x\n", bar0_44);
	seq_printf(s, "  bar0_048_payload2: 0x%08x\n", bar0_48);
	seq_printf(s, "  bar0_04c_payload3: 0x%08x\n", bar0_4c);
	seq_printf(s, "  irq_count: %u\n", hd->irq_count);
	seq_printf(s, "  mailbox_irq_count: %u\n", hd->mailbox_irq_count);
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_puts(s, "next_static_reverse_targets: caller that sets device+0x7698 to stop worker, writers/readers of device+0x29b0 queue slots, and any transition from queued endpoint events to stream object +0x2c8 or persistent physical buffers\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_worker_event_path);

static int hd60pro_windows_event_queue_xrefs_show(struct seq_file *s,
						  void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "windows_event_queue_xrefs: decoded device+0x29b0 event collection; no hardware writes\n");
	seq_puts(s, "queue_init_14028e731: initializes 0x100 entries at device+0x29b0, each 0x40 bytes, with callback 0x14028ee50 and device context as callback data\n");
	seq_puts(s, "queue_lock_14028e70b: initializes lock-like object at device+0x69b8 before queue setup\n");
	seq_puts(s, "queue_event_insert_14028444a: worker queues low BAR0+0x40/+0x44/+0x48 payload token into slots 0x40..0x7f\n");
	seq_puts(s, "queue_event_insert_1402844c5: worker queues high BAR0+0x4c payload token into slots 0x80..0xff\n");
	seq_puts(s, "queue_event_insert_14028ed61: sibling event drain walks slots 0x00..0x3f from BAR0+0x40 bitfields\n");
	seq_puts(s, "queue_event_insert_14028ed9b: sibling event drain walks slots 0x40..0x7f from low payload bits\n");
	seq_puts(s, "queue_event_insert_14028edd5: sibling event drain walks slots 0x80..0xff from high payload bits\n");
	seq_puts(s, "queue_callback_14028ee50: registered callback target for each 0x29b0 slot; next reverse target because it may bridge events to stream state\n");
	seq_puts(s, "nearby_state_helpers_14028fa04_14028fc00: update object+0x18/+0x118 state counters and notify callback 0x14028ec60, using the same +0x69b8 lock family\n");
	seq_puts(s, "directmemory_delivery_140293c39: frame delivery paths lock device+0x1d108, not device+0x29b0, then call 0x14027eb38/0x14027e3b0/0x14027d698\n");
	seq_puts(s, "important_result: +0x29b0 is now modeled as a 256-slot endpoint-event collection fed by BAR0 payloads\n");
	seq_puts(s, "important_negative_result: no decoded +0x29b0 xref writes host physical addresses or frame pixels; frame delivery appears downstream through separate stream/frame objects\n");
	seq_puts(s, "linux_model_status: no Linux event queue is implemented yet; IRQ counts are only diagnostic counters\n");
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "irq_count: %u\n", hd->irq_count);
	seq_printf(s, "mailbox_irq_count: %u\n", hd->mailbox_irq_count);
	seq_puts(s, "next_static_reverse_targets: decode 0x14028ee50 callback and xrefs to device+0x1d108/frame object creation to find the handoff from endpoint events to real frame buffers\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_event_queue_xrefs);

static int hd60pro_windows_event_callback_bridge_show(struct seq_file *s,
						      void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "windows_event_callback_bridge: decoded 0x29b0 callback-to-frame bridge; no hardware writes\n");
	seq_puts(s, "callback_14028ee50_signature_model: called by queue slot callback with rdx=device context, r8=event/token mask, r9=callback payload/context\n");
	seq_puts(s, "callback_14028ee50_locking: takes device+0x69b8 lock via 0x1402a51e0 and releases via 0x1402a51e8\n");
	seq_puts(s, "callback_14028ee50_gate: ignores events when token mask lacks 0x00ff00ff and returns through worker notification path\n");
	seq_puts(s, "callback_14028ef4c_to_14028f4b7: builds temporary arrays of list heads, frame pointers, metadata pointers, flags, and queue output slots from queue object state\n");
	seq_puts(s, "callback_14028f4c9: if stream object +0x10 is nonzero, calls that function pointer through CFG dispatch 0x1402a52d0 with the assembled arrays\n");
	seq_puts(s, "callback_14028f555_to_14028f727: writes callback-returned per-slot values back to queue/list objects and signals wait/event object at slot+0x100\n");
	seq_puts(s, "callback_14028f5b2_board_gate: special-cases subsystem/device words 0x1131:0x7160, 0x1797:0x6801/6804/6810/6811/6812/6813, and 0x14f1:0x8210/0x5851\n");
	seq_puts(s, "callback_14028f6d5: for one 0x1c board/config path, copies a 16-bit event value into delivered frame metadata +0x24\n");
	seq_puts(s, "callback_14028f748: retries after STATUS_INSUFFICIENT_RESOURCES, STATUS_BUFFER_OVERFLOW, or STATUS_UNSUCCESSFUL with masks 0x01000000/0x02000000\n");
	seq_puts(s, "state_helpers_14028fa04_14028fc00: set per-slot state at object+0x18+slot*4+0x118 and optionally notify 0x14028ec60 when object+0x8 changes\n");
	seq_puts(s, "stream_object_alloc_1402381dd: allocates 0x24f0 bytes and stores pointer at device+0x1d110; device+0x1d108 and +0x1d100 are initialized as lock/list heads earlier\n");
	seq_puts(s, "stream_delivery_140293c39_140295720: locks device+0x1d108, removes frame/list entries, resets frame metadata, and calls DirectMemory delivery helpers\n");
	seq_puts(s, "important_result: endpoint BAR0 payloads now have a plausible software path: BAR0 event -> 0x29b0 slot -> 0x14028ee50 -> stream+0x10 callback -> device+0x1d108 frame delivery lists\n");
	seq_puts(s, "important_negative_result: the actual real-capture producer is still behind stream object +0x10; this path handles already-arrived event/frame objects, not BAR programming or physical-address advertisement\n");
	seq_puts(s, "linux_model_status: Linux has no equivalent event queue or stream callback yet; synthetic V4L2 bypasses this path\n");
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "irq_count: %u\n", hd->irq_count);
	seq_printf(s, "mailbox_irq_count: %u\n", hd->mailbox_irq_count);
	seq_printf(s, "v4l2_frames_completed: %u\n", hd->sequence);
	seq_puts(s, "next_static_reverse_targets: identify writer of stream object +0x10, likely during 0x1402381dd stream allocation/init path, then decode that callback as the real handoff from endpoint event records to frame buffers\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_event_callback_bridge);

static int hd60pro_windows_stream_callback_search_show(struct seq_file *s,
						       void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "windows_stream_callback_search: current search state for the stream object +0x10 producer; no hardware writes\n");
	seq_puts(s, "known_invocation_14028f4c9: 0x14028ee50 loads stream object +0x10 into r10 and calls it through CFG dispatch 0x1402a52d0 when endpoint event arrays are ready\n");
	seq_puts(s, "stream_object_alloc_1402381c5_1402381f6: allocates 0x24f0 bytes, stores pointer at device+0x1d110, zeroes the allocation, then uses r14 as the stream object base\n");
	seq_puts(s, "stream_object_list_heads: device+0x1d108 and device+0x1d100 are initialized as lock/list-head objects before stream allocation; DirectMemory frame delivery later locks +0x1d108\n");
	seq_puts(s, "config_init_140238220_plus: stream init repeatedly calls string formatter 0x14022d0c8 and config/property getter 0x14024a768, then stores returned values into stream offsets around +0x1e3c..+0x1ea4\n");
	seq_puts(s, "negative_14022d0c8: decoded as bounded string formatting around 0x140295898, not a callback installer and not DMA programming\n");
	seq_puts(s, "negative_14024a768_call_family: current sampled area reads video/I2C timing registers and computes format timing; not the direct stream+0x10 writer\n");
	seq_puts(s, "negative_direct_store_search: no simple immediate store to [stream+0x10] or [device+0x1d110]+0x10 has been found in the sampled disassembly\n");
	seq_puts(s, "global_selector_note: globals 0x140340ac0/0x140340ac8/0x140340ad0/0x140340ad8 and 0x1402f2d40/0x1402f2d48 are selected during setup, but decoded backing tables at 0x14033ec50/0x14033ed10/0x14033edd0 contain KS/audio-format descriptors and GUIDs, not executable callback pointers\n");
	seq_puts(s, "property_handler_family_14023e760_140241220: many handlers recover device context through 0x1402a50a8/0x1402a50a0, then read/write stream/device fields such as +0x72c0, +0x73b0, +0x8140, +0x8158..+0x8170, +0x1ea4, +0x1f0c, and +0x1fdc\n");
	seq_puts(s, "property_handler_negative: those handlers update video format/routing state and I2C 0x88/0x9c tuning registers; they still do not install stream+0x10 or advertise a host frame physical address\n");
	seq_puts(s, "likely_model: stream+0x10 is installed indirectly through a helper or nested table during stream object/property construction, after device+0x1d110 allocation and before endpoint events are dispatched\n");
	seq_puts(s, "real_capture_implication: Linux should not invent DMA register writes yet; the next real step is to identify this callback target, because it is the consumer that maps endpoint event records into frame delivery lists\n");
	seq_puts(s, "next_static_targets:\n");
	seq_puts(s, "  0x1402381c5..0x140238900 stream object constructor tail and all calls that receive r14/rsi=r14+0x1ea4\n");
	seq_puts(s, "  0x14024a768 full helper and its callees when called from the stream constructor\n");
	seq_puts(s, "  xrefs to CFG callsite 0x14028f4c9 and any store into offsets +0x0/+0x8/+0x10 of stream-owned nested objects\n");
	seq_puts(s, "  callers/callees after 0x140241220 and DirectMemory path 0x140293c39 to find the first object whose +0x10 really is executable\n");
	seq_puts(s, "  import/helper family 0x1402a50a8/0x1402a50a0/0x1402a5150/0x1402a5158 because they recover device context and property objects around stream setup\n");
	seq_puts(s, "linux_status:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  v4l2_registered: %d\n", enable_v4l2);
	seq_printf(s, "  synthetic_v4l2: %d\n", synthetic_v4l2);
	seq_printf(s, "  dma_buffers_prepared: %d\n", hd->dma_frame_cpu ? 1 : 0);
	seq_printf(s, "  real_capture_programmed: 0\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_stream_callback_search);

static int hd60pro_windows_physical_buffer_xrefs_show(struct seq_file *s,
						      void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u16 command = 0;
	unsigned int i;

	pci_read_config_word(hd->pdev, PCI_COMMAND, &command);

	seq_puts(s, "windows_physical_buffer_xrefs: decoded persistent physical buffer field xrefs; no hardware writes\n");
	seq_puts(s, "device_context_register: r14 in 0x14028d250..0x14028e9ee\n");
	seq_puts(s, "init_zero_14028d2f9: device+0xc8 physical=0, device+0xd0 virtual=0, device+0xe0 size=0\n");
	seq_puts(s, "init_zero_14028d342: device+0x138 physical=0, device+0x140 virtual=0, device+0x148 size=0\n");
	seq_puts(s, "control_alloc_14028e19c: MmAllocateContiguousMemorySpecifyCache stores virtual at device+0xd0\n");
	seq_puts(s, "control_phys_14028e1b7: MmGetPhysicalAddress stores physical at device+0xc8\n");
	seq_puts(s, "control_cleanup_14028e81c: frees device+0xd0 using physical address device+0xc8 and size device+0xe0 when DMA adapter object is active\n");
	seq_puts(s, "status_alloc_14028e2a2: MmAllocateContiguousMemorySpecifyCache stores virtual at device+0x140\n");
	seq_puts(s, "status_phys_14028e2bd: MmGetPhysicalAddress stores physical at device+0x138\n");
	seq_puts(s, "status_cleanup_14028e8a6: frees device+0x140 using size device+0x148\n");
	seq_puts(s, "channel_alloc_14028e4db: stores per-channel virtual pointer at device+0x1190 + channel_index*8\n");
	seq_puts(s, "channel_phys_14028e522: stores per-channel physical address at device+0x190 + channel_index*0x10\n");
	seq_puts(s, "channel_meta_14028e52f: stores per-channel size/metadata at device+0x198 + channel_index*0x10\n");
	seq_puts(s, "channel_fallback_14028e5c3: alternate contiguous allocation path writes the same virtual/physical/metadata fields\n");
	seq_puts(s, "channel_cleanup_14028e8f3: walks device+0x1190 and frees channel allocations, clearing device+0x190/+0x198 metadata families\n");
	seq_puts(s, "channel_consumer_14027b977: selects device+0x1190-family buffer by channel/slot and returns a host pointer for later frame handling\n");
	seq_puts(s, "channel_consumer_14027fb79: selects device+0x1190-family buffer by modulo slot; used in DirectMemory frame assembly\n");
	seq_puts(s, "channel_consumer_14028026f: reads 0x1190-family buffers and interleaves/copies byte/word planes into derived host buffers\n");
	seq_puts(s, "negative_xref_result: no direct xref in this block writes device+0xc8/+0x138/+0x190 to BAR0/BAR5; those addresses likely flow through a later adapter/DMA callback or endpoint event path\n");
	seq_puts(s, "channel_negative_result: decoded 0x1190 consumers are host buffer selection/copy paths, not endpoint outbound-window programming\n");
	seq_puts(s, "linux_mirror_status: allocated only when prepare_dma_buffers=1; not advertised to hardware\n");
	seq_printf(s, "linux_pci_bus_master_enabled: %d\n",
		   !!(command & PCI_COMMAND_MASTER));
	seq_printf(s, "linux_control_cpu_device_0xd0: %px\n",
		   hd->win_dma_control_cpu);
	seq_printf(s, "linux_control_dma_device_0xc8: %pad\n",
		   &hd->win_dma_control_dma);
	seq_printf(s, "linux_control_size_device_0xe0: %zu\n",
		   hd->win_dma_control_size);
	seq_printf(s, "linux_status_cpu_device_0x140: %px\n",
		   hd->win_dma_status_cpu);
	seq_printf(s, "linux_status_dma_device_0x138: %pad\n",
		   &hd->win_dma_status_dma);
	seq_printf(s, "linux_status_size_device_0x148: %zu\n",
		   hd->win_dma_status_size);
	for (i = 0; i < HD60PRO_WINDOWS_DMA_CHANNELS; i++) {
		seq_printf(s, "linux_channel%u_cpu_device_0x1190: %px\n",
			   i, hd->win_dma_channel_cpu[i]);
		seq_printf(s, "linux_channel%u_dma_device_0x190: %pad\n",
			   i, &hd->win_dma_channel_dma[i]);
		seq_printf(s, "linux_channel%u_meta_size_device_0x198: %zu\n",
			   i, hd->win_dma_channel_size[i]);
	}
	seq_puts(s, "next_static_reverse_targets: xrefs to post-init paths that consume device+0xc8/+0x138/+0x190; 0x1402a52d0 is CFG dispatch, not a DMA import\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_physical_buffer_xrefs);

static int hd60pro_firmware_userland_flow_show(struct seq_file *s,
					       void *unused)
{
	seq_puts(s, "firmware_userland_flow: decoded from yuan_demo_sdi/video_capture_mgr\n");
	seq_puts(s, "event_fd: opens /sys/vpl_pciep/epint with O_RDWR\n");
	seq_puts(s, "event_loop: read 0x2c bytes, poll forever, pread 0x2c bytes at offset 0, dispatch on event[0]\n");
	seq_puts(s, "event_ack: handled events are pwrite() back as 0x2c-byte records at offset 0\n");
	seq_puts(s, "event_0x29: SET_VIC path; validates width/height > 0x7f, stores per-channel capture config, starts tinyvenc path\n");
	seq_puts(s, "event_0x2a: Set AIC params path for audio capture state\n");
	seq_puts(s, "event_0x60: LOGO BEGIN DOWNLOAD path; saves channel/logo type/firmware size/logo order and acks\n");
	seq_puts(s, "event_0x61: LOGO END DOWNLOAD path; finalizes saved logo state and acks\n");
	seq_puts(s, "event_0x6e: LOAD_FILES path; may transform payload bytes then ack\n");
	seq_puts(s, "event_0x07: STOP_STREAMING path; runs echo '0' > /sys/vpl_pciep/hready and kills capture_app_infinite/capture_audio_8ch\n");
	seq_puts(s, "set_vic_format_string: [Video_MGR][ch%d] SET_VIC fw(%d), fps(%d), resolution(%dx%d) interlace(%d), ... input_frame_width(%d), input_frame_height(%d), bitstream_num(%d), ... is_nosg(%d)\n");
	seq_puts(s, "host_implication: Linux host probably needs to make firmware emit/consume epint records and hready state, not only mailbox A3 queries\n");
	seq_puts(s, "next_reverse_target: correlate Windows stream-start writes with the epint 0x29/0x2a record layout and the 0x2c-byte ack ownership protocol\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_firmware_userland_flow);

static int hd60pro_endpoint_bridge_regs_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	static const u8 regs[] = {
		0x15, 0x16, 0x18, 0x40,
		0x8d, 0x8e, 0x8f, 0x92, 0x93, 0x94, 0x95,
	};
	unsigned int i;
	u32 value = 0;
	int ret;

	seq_puts(s, "endpoint_bridge_regs: safe read-only probe for Windows 0x88 bridge helper registers\n");
	seq_puts(s, "source: Windows helpers 0x1402777e4(read 0x1a) and 0x1402851cc(write 0x1b) called during stream-start/update paths\n");
	seq_puts(s, "windows_read_helper_packet: [0x800,0x1a,chip,reg,0], returns BAR0+0x10 low byte\n");
	seq_puts(s, "windows_write_helper_packet: [0x800,0x1b,chip,reg,value]\n");
	seq_puts(s, "chip: 0x88\n");
	if (!allow_mailbox_writes || !allow_i2c_read_command1a) {
		seq_puts(s, "blocked; reload with allow_mailbox_writes=1 allow_i2c_read_command1a=1\n");
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		char name[32];

		snprintf(name, sizeof(name), "read_88_%02x", regs[i]);
		ret = hd60pro_i2c_read8_locked(hd, s, name, 0x88, regs[i],
					       &value);
		if (ret)
			break;
	}
	mutex_unlock(&hd->mailbox_lock);

	seq_puts(s, "interpretation: 0x15/0x16/0x18 are read and recomputed by Windows after resolution changes; 0x40/0x8d..0x95 are read by the Windows status/event helper at 0x14026edd8\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_endpoint_bridge_regs);

static int hd60pro_firmware_pcie_outbound_regs_show(struct seq_file *s,
						    void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 reg50 = hd->bar5 && hd->bar5_len >= 0x54 ?
		    ioread32(hd->bar5 + 0x50) : U32_MAX;
	u32 reg54 = hd->bar5 && hd->bar5_len >= 0x58 ?
		    ioread32(hd->bar5 + 0x54) : U32_MAX;
	u32 reg58 = hd->bar5 && hd->bar5_len >= 0x5c ?
		    ioread32(hd->bar5 + 0x58) : U32_MAX;
	u32 reg74 = hd->bar5 && hd->bar5_len >= 0x78 ?
		    ioread32(hd->bar5 + 0x74) : U32_MAX;
	u32 reg7c = hd->bar5 && hd->bar5_len >= 0x80 ?
		    ioread32(hd->bar5 + 0x7c) : U32_MAX;
	u32 regd4 = hd->bar5 && hd->bar5_len >= 0xd8 ?
		    ioread32(hd->bar5 + 0xd4) : U32_MAX;

	seq_puts(s, "firmware_pcie_outbound_regs: read-only BAR5 comparison against firmware ep.ko pcie_set_outbound\n");
	seq_puts(s, "source: yuan_demo_sdi/drivers/ep.ko, ARM symbol pcie_set_outbound at .text+0x5a8 size 0xa0\n");
	seq_puts(s, "symbol_status: ep.ko is ARM32 relocatable and not stripped; local host lacks ARM objdump, so this is from readelf symbols plus xxd/manual ARM instruction decoding\n");
	seq_puts(s, "decoded_sequence:\n");
	seq_puts(s, "  select channel window based on caller arg: channel 0/1/2/3 maps to base offsets +0x00/+0x14/+0x08/+0x0c from a bss channel table\n");
	seq_puts(s, "  write selected_channel+0x50 = 0x00000001\n");
	seq_puts(s, "  write selected_channel+0x74 = 0x90000000 (instruction bytes 09 22 a0 e3)\n");
	seq_puts(s, "  write selected_channel+0x7c = 0x91ffffff (instruction bytes 6e 24 e0 e3, mvn-immediate form)\n");
	seq_puts(s, "  write selected_channel+0x54 = caller arg0 low word\n");
	seq_puts(s, "  write selected_channel+0x58 = caller arg1 low word\n");
	seq_puts(s, "  write selected_channel+0xd4 = 0x0f000000 (instruction bytes 0f 26 a0 e3)\n");
	seq_puts(s, "linux_bar5_current:\n");
	seq_printf(s, "  bar5_050: 0x%08x\n", reg50);
	seq_printf(s, "  bar5_054: 0x%08x\n", reg54);
	seq_printf(s, "  bar5_058: 0x%08x\n", reg58);
	seq_printf(s, "  bar5_074: 0x%08x\n", reg74);
	seq_printf(s, "  bar5_07c: 0x%08x\n", reg7c);
	seq_printf(s, "  bar5_0d4: 0x%08x\n", regd4);
	seq_puts(s, "linux_policy: do not write these from Linux yet; next step is matching the Windows host call that supplies pcie_set_outbound arg0/arg1 equivalents\n");
	seq_puts(s, "next_reverse_targets: firmware callers of pcie_set_outbound, video_capture_mgr epint record fields that lead to it, and Windows mailbox/BAR payloads carrying those two address words\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_firmware_pcie_outbound_regs);

static int hd60pro_firmware_dmac_outbound_path_show(struct seq_file *s,
						    void *unused)
{
	struct hd60pro_dev *hd = s->private;

	seq_puts(s, "firmware_dmac_outbound_path: decoded firmware caller chain for pcie_set_outbound; no hardware writes\n");
	seq_puts(s, "source_modules: yuan_demo_sdi/drivers/vpl_dmac.ko and ep.ko\n");
	seq_puts(s, "symbol_pcie_set_outbound: ep.ko .text+0x5a8 size 0xa0, exported and imported by vpl_dmac.ko\n");
	seq_puts(s, "vpl_dmac_relocations:\n");
	seq_puts(s, "  .rel.text+0x00000c94: R_ARM_CALL pcie_set_outbound inside VPL_DMAC_StartTail\n");
	seq_puts(s, "  .rel.text+0x00000f70: R_ARM_CALL pcie_set_outbound inside VPL_DMAC_ISRTail\n");
	seq_puts(s, "VPL_DMAC_StartTail_0x0b5c:\n");
	seq_puts(s, "  computes selected profile slot from channel/index masked by 0x3f\n");
	seq_puts(s, "  copies profile words +0x08..+0x34 into the selected DMAC MMR window\n");
	seq_puts(s, "  ORs a start/control bit into MMR +0x08 after the copy\n");
	seq_puts(s, "  if profile byte +0x3b allows outbound setup, calls pcie_set_outbound(profile+0x38, profile+0x39, profile+0x3a)\n");
	seq_puts(s, "VPL_DMAC_ISRTail_0x0e5c:\n");
	seq_puts(s, "  mirrors the same profile-to-MMR copy during tail interrupt handling\n");
	seq_puts(s, "  if the tail profile gate allows it, calls pcie_set_outbound(profile+0x38, profile+0x39, profile+0x3a)\n");
	seq_puts(s, "interpretation:\n");
	seq_puts(s, "  the missing host DMA advertisement is probably not a Windows BAR write directly to BAR5+0x54/+0x58\n");
	seq_puts(s, "  Windows likely causes firmware video_capture_mgr/libvideocap to build a VPL_DMAC tail profile, and vpl_dmac.ko then programs the PCI endpoint outbound window\n");
	seq_puts(s, "  profile bytes +0x38/+0x39/+0x3a are now critical; they should correspond to endpoint channel and host/outbound address selector words\n");
	seq_puts(s, "linux_next_step:\n");
	seq_puts(s, "  recover the firmware userspace ioctl/profile struct passed to VPL_DMAC_StartTail from libvideocap.so.13 VideoCap_StartVIC/GetBufVIC paths\n");
	seq_puts(s, "  only after that, add a guarded Linux mirror that writes an equivalent DMAC tail profile or endpoint event instead of guessing BAR5 writes\n");
	seq_puts(s, "linux_current_state:\n");
	seq_printf(s, "  pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "  v4l2_synthetic_frames: %d\n", synthetic_v4l2);
	seq_printf(s, "  dma_buffers_prepared: %d\n",
		   hd->dma_desc_cpu && hd->dma_frame_cpu);
	seq_printf(s, "  real_dma_programmed: 0\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_firmware_dmac_outbound_path);

static int hd60pro_windows_88_update_plan_show(struct seq_file *s,
					       void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u32 r15 = 0, r16 = 0, r18 = 0;
	int ret = 0;

	seq_puts(s, "windows_88_update_plan: no hardware writes; decoded Windows stream-start register update\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS 0x14026ec85..0x14026ed36\n");
	seq_puts(s, "mailbox_helpers: read=0x1402777e4 -> [0x800,0x1a,0x88,reg,0], write=0x1402851cc -> [0x800,0x1b,0x88,reg,value]\n");
	seq_puts(s, "windows_sequence: read 0x88:18, read 0x88:16, read 0x88:15, compute adjusted values using context+0x36c/+0xa20 calibration tables, write 0x88:15, write 0x88:16, write 0x88:18\n");
	seq_puts(s, "windows_formula_observed:\n");
	seq_puts(s, "  base_phase = ((read15 & 0x70) << 4) | read16\n");
	seq_puts(s, "  base_gain = read18\n");
	seq_puts(s, "  adjusted_phase = base_phase + calibration_36c[index]\n");
	seq_puts(s, "  adjusted_gain = base_gain + calibration_a20[index]\n");
	seq_puts(s, "  write15 = ((adjusted_phase >> 4) & 0x70) | (read15 & 0x0f)\n");
	seq_puts(s, "  write16 = adjusted_phase & 0xff\n");
	seq_puts(s, "  write18 = adjusted_gain & 0xff\n");
	seq_puts(s, "local_observation: current pipeline-ready card reports all three source registers as zero\n");
	if (!allow_mailbox_writes || !allow_i2c_read_command1a) {
		seq_puts(s, "live_read_blocked; reload with allow_mailbox_writes=1 allow_i2c_read_command1a=1\n");
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_i2c_read8_locked(hd, s, "source_read_88_18", 0x88,
				       0x18, &r18);
	if (!ret)
		ret = hd60pro_i2c_read8_locked(hd, s, "source_read_88_16",
					       0x88, 0x16, &r16);
	if (!ret)
		ret = hd60pro_i2c_read8_locked(hd, s, "source_read_88_15",
					       0x88, 0x15, &r15);
	mutex_unlock(&hd->mailbox_lock);

	if (!ret) {
		u32 base_phase = ((r15 & 0x70) << 4) | (r16 & 0xff);

		seq_printf(s, "observed_read15: 0x%02x\n", r15 & 0xff);
		seq_printf(s, "observed_read16: 0x%02x\n", r16 & 0xff);
		seq_printf(s, "observed_read18: 0x%02x\n", r18 & 0xff);
		seq_printf(s, "observed_base_phase: 0x%03x\n", base_phase);
		seq_printf(s, "observed_base_gain: 0x%02x\n", r18 & 0xff);
	}
	seq_puts(s, "next_needed: recover calibration_36c/a20 index/value source from the Windows context or trace these writes on Windows\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_88_update_plan);

static int hd60pro_windows_calibration_info_show(struct seq_file *s,
						 void *unused)
{
	seq_puts(s, "windows_calibration_info: decoded Windows calibration property paths; no hardware writes\n");
	seq_puts(s, "source_binary: e60MZ0380.X64.SYS\n");
	seq_puts(s, "streaming_gate: context+0x72c0 == 1 and context+0x1d110 stream-info pointer must be valid\n");
	seq_puts(s, "current_index: stream_info+0x1fdc selects the active calibration slot\n");
	seq_puts(s, "mode_selector: context+0x73a8 selects table family; mode 3 uses primary tables, mode 2 uses alternate tables\n");
	seq_puts(s, "board_gate: for other modes Windows checks PCI/private byte context->board+0x0e against 0x51/0x52 or 0xc1/0xc2 before using primary tables\n");
	seq_puts(s, "\nphase_table_getter_14023f0a0:\n");
	seq_puts(s, "  mode3_value: stream_info[index]+0x36c\n");
	seq_puts(s, "  mode2_value: stream_info[index]+0x1e3c\n");
	seq_puts(s, "  otherwise: zero unless board_gate passes, then primary +0x36c\n");
	seq_puts(s, "phase_table_setter_14023f120:\n");
	seq_puts(s, "  stores user value to +0x36c or +0x1e3c and marks +0x1788/+0x1f74 dirty flags\n");
	seq_puts(s, "  updates aggregate phase stream_info+0x1fe0 + user value\n");
	seq_puts(s, "  when aggregate >= 0, writes chip 0x9c reg 0x80 high nibble/phase-high and reg 0x81 phase-low\n");
	seq_puts(s, "  if endpoint bridge exists at context+0x1f1a8 and stream_info+0x2080 matches 0x260c/0x270c, writes chip 0x88:40=channel, reads 0x88:16/15, writes 0x88:15/16\n");
	seq_puts(s, "\ngain_table_getter_14023f320:\n");
	seq_puts(s, "  mode3_value: stream_info[index]+0xa20\n");
	seq_puts(s, "  mode2_value: stream_info[index]+0x1ea4\n");
	seq_puts(s, "  otherwise: zero unless board_gate passes, then primary +0xa20\n");
	seq_puts(s, "gain_table_setter_14023f3a0:\n");
	seq_puts(s, "  stores user value to +0xa20 or +0x1ea4 and marks +0x1788/+0x1f74 dirty flags\n");
	seq_puts(s, "  updates aggregate gain stream_info+0x1fe4 + user value\n");
	seq_puts(s, "  when aggregate >= 0, writes chip 0x9c reg 0x84 with low byte of aggregate gain\n");
	seq_puts(s, "  if endpoint bridge exists and stream_info+0x2080 matches 0x260c/0x270c, writes chip 0x88:40=channel, reads 0x88:18, writes 0x88:18=aggregate gain low byte\n");
	seq_puts(s, "\ncapture_setup_consumers:\n");
	seq_puts(s, "  0x1402518aa reads stream_info[index]+0x36c during stream setup\n");
	seq_puts(s, "  0x1402518b2 reads stream_info[index]+0xa20 during stream setup\n");
	seq_puts(s, "  0x14026ec85..0x14026ed36 recomputes live 0x88:15/16/18 from current hardware reads plus those two calibration values\n");
	seq_puts(s, "\nlinux_status:\n");
	seq_puts(s, "  calibration defaults are not recovered yet; current live 0x88 reads are zero on this card after pipeline-ready init\n");
	seq_puts(s, "  keep 0x88 writes disabled until we know whether Windows sets nonzero calibration values or zero is a valid default for this board/mode\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_calibration_info);

static int hd60pro_windows_bridge_attach_info_show(struct seq_file *s,
						   void *unused)
{
	struct hd60pro_dev *hd = s->private;
	static const u8 chip60_regs[] = {
		0xff, 0xf0, 0xf2, 0xf3, 0xf4, 0xf5, 0x5c,
	};
	unsigned int i;
	u32 value = 0;
	int ret;

	seq_puts(s, "windows_bridge_attach_info: decoded Windows endpoint-bridge attach paths; read-only except optional live mailbox reads\n");
	seq_puts(s, "source_binary: e60MZ0380.X64.SYS\n");
	seq_puts(s, "attach_path_140254614: iterates four sibling contexts from global table 0x14039f160 + context+0x1f190\n");
	seq_puts(s, "attach_path_140254614: if sibling+0x1f1a8 is null, calls post-logo helper 0x140287224, then stores sibling+0x1f1a8=parent_context and sibling+0x1f1b0=channel_index\n");
	seq_puts(s, "attach_path_14026dec0: stream/status worker repeats the same sibling+0x1f1a8/+0x1f1b0 attach for active channels\n");
	seq_puts(s, "board_gates: stream_info+0x2080 values 0x2400/0x2611/0x270c alter channel count and mode; second+ channels require board byte 0x52 or 0xc2\n");
	seq_puts(s, "after_attach_chip60_sequence_14025469c:\n");
	seq_puts(s, "  write 0x60:ff = (channel_index & 3) + 5\n");
	seq_puts(s, "  read 0x60:f0/f4/f5, then conditionally read 0x60:f3/f2\n");
	seq_puts(s, "  maps observed status bytes through helpers 0x14025e57c/0x14025e544/0x140254884\n");
	seq_puts(s, "  writes 0x60:ff again with the same selector before checking 0x60:5c\n");
	seq_puts(s, "  may call GPIO/helper 0x14025d87c with line 0x12 and 0x24 after 1s/100ms/100ms sleeps when state becomes 0x31/0x32\n");
	seq_puts(s, "consumers_1402441a0_140244210: property/query paths call 0x14026edd8 and 0x14026fa98 through context+0x1f1a8 with channel context+0x1f1b0\n");
	seq_puts(s, "linux_inference: 0x1f1a8 is not a DMA buffer pointer; it is a parent/controller context used for cross-channel bridge I2C/mailbox accesses\n");
	seq_puts(s, "next_needed: decode 0x14026fa98 and 0x14027379c/0x1402737d4 to identify the host-visible event or hready/channel_done transition\n");

	if (!allow_mailbox_writes || !allow_i2c_read_command1a) {
		seq_puts(s, "live_chip60_read_blocked; reload with allow_mailbox_writes=1 allow_i2c_read_command1a=1\n");
		return 0;
	}

	seq_puts(s, "live_chip60_reads:\n");
	mutex_lock(&hd->mailbox_lock);
	for (i = 0; i < ARRAY_SIZE(chip60_regs); i++) {
		char name[32];

		snprintf(name, sizeof(name), "read_60_%02x",
			 chip60_regs[i]);
		ret = hd60pro_i2c_read8_locked(hd, s, name, 0x60,
					       chip60_regs[i], &value);
		if (ret)
			break;
	}
	mutex_unlock(&hd->mailbox_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_bridge_attach_info);

struct hd60pro_reg8_pair {
	u8 reg;
	u8 value;
};

static const u8 hd60pro_win_88_rmw_regs_1402d5984[] = {
	0xfa, 0xfa, 0xfb, 0xfb, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};

static const u8 hd60pro_win_88_rmw_masks_1402d59f4[] = {
	0xf8, 0x8f, 0xf8, 0x8f, 0x00, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0x00, 0x02, 0x03, 0x00, 0x00,
};

static const u8 hd60pro_win_88_rmw_or_1402d5af0[] = {
	0x01, 0x10, 0x01, 0x10, 0x01, 0x02, 0x04, 0x08,
	0x0f, 0x00, 0x00, 0x00, 0xfe, 0xfd, 0xfb, 0xf7,
};

static const u8 hd60pro_win_88_f5_masks_1402d5afc[] = {
	0xfe, 0xfd, 0xfb, 0xf7, 0xf0, 0x00, 0x00, 0x00,
};

static const u8 hd60pro_win_88_literal_14033f770_first16[] = {
	0xe9, 0x4b, 0x81, 0x6f, 0xf6, 0x9a, 0xcf, 0x43,
	0x92, 0x49, 0xc0, 0x34, 0x18, 0x01, 0x02, 0x22,
};

static const u8 hd60pro_win_88_literal_14033f7a0_first16[] = {
	0xe9, 0x4b, 0x81, 0x6f, 0xf6, 0x9a, 0xcf, 0x43,
	0x92, 0x49, 0xc0, 0x34, 0x21, 0x01, 0x02, 0x22,
};

static void hd60pro_seq_u8_table(struct seq_file *s, const char *name,
				 const u8 *table, unsigned int count)
{
	unsigned int i;

	seq_printf(s, "%s:", name);
	for (i = 0; i < count; i++)
		seq_printf(s, " %02x", table[i]);
	seq_putc(s, '\n');
}

static int hd60pro_windows_88_mask_tables_show(struct seq_file *s,
					       void *unused)
{
	unsigned int i;

	seq_puts(s, "windows_88_mask_tables: decoded Windows chip 0x88 dynamic mask/update helpers; no hardware writes\n");
	seq_puts(s, "source_binary: e60MZ0380.X64.SYS .rdata extracted by VA to PE section offset mapping\n");
	seq_puts(s, "helper_clear_14027383c: reads chip 0x88 register selected by table 0x1402d5984, writes old & mask from 0x1402d59f4\n");
	seq_puts(s, "helper_set_1402739ac: same selector/mask path, then ORs table byte from 0x1402d5af0 before writing\n");
	seq_puts(s, "helper_literal_140272d20: writes count bytes from caller table to consecutive chip 0x88 registers starting at base dl\n");
	seq_puts(s, "helper_literal_call_a: 0x14027282e uses table 0x14033f770, base 0x15, count 9\n");
	seq_puts(s, "helper_literal_call_b: 0x140272a74 uses table 0x14033f7a0, base 0x15, count 9\n");
	seq_puts(s, "helper_literal_note: these tables look like repeated GUID/property literals in .rdata, but this helper consumes their first bytes as hardware write payload\n");
	seq_puts(s, "stream_id_group_a: 0x2400, 0x2611, 0x270c use four consecutive table slots when channel selector >= 4\n");
	seq_puts(s, "stream_id_group_b: 0x2601, 0x260c use two table slots with even indexing when channel selector >= 4\n");
	seq_puts(s, "stream_id_group_small_channel: selector < 4 uses either the direct selector or (selector & 1) * 2 depending stream family\n");
	seq_puts(s, "helper_select_1402737d4: writes chip 0x88:40 with a Windows channel/mode selector mapping before dynamic 0x88 accesses\n");
	seq_puts(s, "helper_select_1402737d4_mapping: selector 0->00 1->01 2->02 3->03 4->04 5->10 9->40, other high selectors fall back through value 04 unless exact Windows compare passes\n");
	seq_puts(s, "helper_encode_1402734d8: converts one byte into five consecutive 0x88 writes: base=00, base+1=encoded_mode, base+2=encoded_mid, base+3=encoded_tail, base+4=fc\n");
	seq_puts(s, "property_caller_140244210: takes caller r8 as an 8-byte state buffer, uses child context+0x1f1a8 parent bridge and child context+0x1f1b0 channel, then calls 0x14026fa98\n");
	seq_puts(s, "status_caller_1402441a0: similar bridge wrapper, but calls 0x14026edd8 to read eight 0x88 status bytes into caller r8\n");
	seq_puts(s, "helper_state_14026fa98: consumes the 8-byte per-channel state buffer and writes dynamic 0x88 windows around 0x56..0x80, gated by stream_info+0x2080\n");
	seq_puts(s, "helper_state_14026fa98_context: reads parent context+0x81a4 mode; if >=3, selector defaults can come from stream_info+channel*4+0x22f4 and state byte from +0x2304\n");
	seq_puts(s, "helper_state_14026fa98_default_writes: for non-0x2601/0x260c/0x2611/0x270c stream IDs, clears 0x88:40 then writes dynamic regs 0x56/57/5b/5c, selects 0x88:40=0x10, clears same regs again, then writes input bytes to 0x58..0x5f style windows\n");
	seq_puts(s, "helper_state_14026fa98_special_writes: 0x2601/0x2611 and 0x260c/0x270c branches use helper_encode_1402734d8 for packed pattern writes instead of only direct byte writes\n");
	hd60pro_seq_u8_table(s, "table_1402d5984_regs_first16",
			     hd60pro_win_88_rmw_regs_1402d5984,
			     ARRAY_SIZE(hd60pro_win_88_rmw_regs_1402d5984));
	hd60pro_seq_u8_table(s, "table_1402d59f4_and_masks_first16",
			     hd60pro_win_88_rmw_masks_1402d59f4,
			     ARRAY_SIZE(hd60pro_win_88_rmw_masks_1402d59f4));
	hd60pro_seq_u8_table(s, "table_1402d5af0_or_bits_first16",
			     hd60pro_win_88_rmw_or_1402d5af0,
			     ARRAY_SIZE(hd60pro_win_88_rmw_or_1402d5af0));
	hd60pro_seq_u8_table(s, "table_1402d5afc_f5_masks_first8",
			     hd60pro_win_88_f5_masks_1402d5afc,
			     ARRAY_SIZE(hd60pro_win_88_f5_masks_1402d5afc));
	hd60pro_seq_u8_table(s, "literal_14033f770_first16",
			     hd60pro_win_88_literal_14033f770_first16,
			     ARRAY_SIZE(hd60pro_win_88_literal_14033f770_first16));
	hd60pro_seq_u8_table(s, "literal_14033f7a0_first16",
			     hd60pro_win_88_literal_14033f7a0_first16,
			     ARRAY_SIZE(hd60pro_win_88_literal_14033f7a0_first16));
	seq_puts(s, "literal_14033f770_write_plan_count9: 88:15=e9 88:16=4b 88:17=81 88:18=6f 88:19=f6 88:1a=9a 88:1b=cf 88:1c=43 88:1d=92\n");
	seq_puts(s, "literal_14033f7a0_write_plan_count9: same first nine bytes as 0x14033f770; later table bytes differ but are not consumed by those two count=9 calls\n");
	seq_puts(s, "decoded_slot_plan_first_four:\n");
	for (i = 0; i < 4; i++) {
		u8 reg = hd60pro_win_88_rmw_regs_1402d5984[i];
		u8 mask = hd60pro_win_88_rmw_masks_1402d59f4[i];
		u8 set = hd60pro_win_88_rmw_or_1402d5af0[i];

		seq_printf(s, "  slot_%u: reg=0x%02x clear=(old & 0x%02x) set=((old & 0x%02x) | 0x%02x)\n",
			   i, reg, mask, mask, set);
	}
	seq_puts(s, "local_status: these tables explain the previously skipped 0x88:f5 mask family and the post-preset dynamic fa/fb updates; 0x14026fa98 appears to be a dynamic property/state setter, not the missing DMA transport itself\n");
	seq_puts(s, "next_needed: identify the KS/property that feeds the 8-byte r8 buffer, then separate optional signal-conditioning writes from mandatory stream-start/DMA endpoint commands\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_88_mask_tables);

static int hd60pro_windows_capture_88_presets_show(struct seq_file *s,
						   void *unused)
{
	static const struct hd60pro_reg8_pair preset_272d98[] = {
		{ 0x0c, 0x03 }, { 0x0d, 0x10 }, { 0x20, 0x60 },
		{ 0x26, 0x02 }, { 0x2b, 0x58 }, { 0x2d, 0x30 },
		{ 0x2e, 0x70 }, { 0x30, 0x48 }, { 0x31, 0xbb },
		{ 0x32, 0x2e }, { 0x33, 0x90 }, { 0x39, 0x8c },
		{ 0x2c, 0x0a }, { 0x27, 0x2d }, { 0x28, 0x00 },
		{ 0x13, 0x00 },
	};
	static const struct hd60pro_reg8_pair preset_272f1c[] = {
		{ 0x0c, 0x03 }, { 0x0d, 0x10 }, { 0x20, 0x60 },
		{ 0x26, 0x02 }, { 0x2b, 0x58 }, { 0x2d, 0x30 },
		{ 0x2e, 0x70 }, { 0x30, 0x48 }, { 0x31, 0xbb },
		{ 0x32, 0x2e }, { 0x33, 0x90 }, { 0x39, 0x88 },
		{ 0x2c, 0x0a }, { 0x27, 0x2d }, { 0x28, 0x00 },
		{ 0x13, 0x00 },
	};
	unsigned int i;

	seq_puts(s, "windows_capture_88_presets: decoded Windows chip 0x88 capture/video presets; no hardware writes\n");
	seq_puts(s, "source_binary: e60MZ0380.X64.SYS\n");
	seq_puts(s, "dispatch_source: 0x140272a0f chooses helpers by stream_info+0x2080 and mode/channel state\n");
	seq_puts(s, "mode_ids_seen: 0x2400, 0x2601, 0x2611, 0x260c, 0x270c\n");
	seq_puts(s, "common_entry: writes 0x88:35=0x05 when stream_info+0x2080 >= 0x2200\n");
	seq_puts(s, "common_entry: reads 0x88:02 then writes (read & 0xfa) | 0x02 back to 0x88:02\n");
	seq_puts(s, "common_entry: reads 0x88:f5 and masks it with table byte at 0x1402d5afc[index], then writes 0x88:f5; first bytes are fe fd fb f7 f0\n");
	seq_puts(s, "preset_140272d98_for_0x2400:\n");
	for (i = 0; i < ARRAY_SIZE(preset_272d98); i++)
		seq_printf(s, "  write_88_%02x: 0x%02x\n",
			   preset_272d98[i].reg, preset_272d98[i].value);
	seq_puts(s, "  final_rmw_88_14: read 0x88:14, write value & 0x9f\n");
	seq_puts(s, "preset_140272f1c_variant:\n");
	for (i = 0; i < ARRAY_SIZE(preset_272f1c); i++)
		seq_printf(s, "  write_88_%02x: 0x%02x\n",
			   preset_272f1c[i].reg, preset_272f1c[i].value);
	seq_puts(s, "  final_rmw_88_14: read 0x88:14, write value & 0x9f\n");
	seq_puts(s, "special_1080p_like_tail_140272bc3_when_mode_byte_2_and_stream_id_matches_0x2601_family_or_0x270c:\n");
	seq_puts(s, "  write_88_16: 0x40\n");
	seq_puts(s, "  write_88_15: 0x13\n");
	seq_puts(s, "  write_88_16: 0x0a\n");
	seq_puts(s, "  write_88_17: 0x00\n");
	seq_puts(s, "  write_88_18: 0x19\n");
	seq_puts(s, "  write_88_19: 0xd0\n");
	seq_puts(s, "  write_88_1a: 0x25\n");
	seq_puts(s, "  write_88_1c: 0x06\n");
	seq_puts(s, "  write_88_1d: 0x7a\n");
	seq_puts(s, "post_tail: calls 0x14027383c and optionally sets bit 0x40 in 0x88:35 if state flag byte has 0x40\n");
	seq_puts(s, "linux_status: captured as documentation only; do not auto-apply before stream_info+0x2080 mode and source signal path are confirmed\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_windows_capture_88_presets);

static int hd60pro_capture_88_dump_regs_locked(struct hd60pro_dev *hd,
					       struct seq_file *s,
					       const char *prefix)
{
	static const u8 regs[] = {
		0x02, 0x0c, 0x0d, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x20, 0x26, 0x27,
		0x28, 0x2b, 0x2c, 0x2d, 0x2e, 0x30, 0x31, 0x32,
		0x33, 0x35, 0x39, 0xf5,
	};
	unsigned int i;
	u32 value = 0;
	int ret;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		char name[40];

		snprintf(name, sizeof(name), "%s_88_%02x", prefix, regs[i]);
		ret = hd60pro_i2c_read8_locked(hd, s, name, 0x88, regs[i],
					       &value);
		if (ret)
			return ret;
	}

	return 0;
}

static int hd60pro_apply_capture_88_preset_2400_min_show(struct seq_file *s,
							 void *unused)
{
	struct hd60pro_dev *hd = s->private;
	static const struct hd60pro_reg8_pair preset[] = {
		{ 0x35, 0x05 },
		{ 0x0c, 0x03 }, { 0x0d, 0x10 }, { 0x20, 0x60 },
		{ 0x26, 0x02 }, { 0x2b, 0x58 }, { 0x2d, 0x30 },
		{ 0x2e, 0x70 }, { 0x30, 0x48 }, { 0x31, 0xbb },
		{ 0x32, 0x2e }, { 0x33, 0x90 }, { 0x39, 0x8c },
		{ 0x2c, 0x0a }, { 0x27, 0x2d }, { 0x28, 0x00 },
		{ 0x13, 0x00 },
	};
	u32 value02 = 0, value14 = 0;
	unsigned int i;
	int ret = 0;

	seq_puts(s, "apply_capture_88_preset_2400_min: experimental opt-in Windows 0x88 preset; hardware writes when enabled\n");
	seq_puts(s, "source: e60MZ0380.X64.SYS helpers 0x140272a0f and 0x140272d98\n");
	seq_puts(s, "scope: common 0x88:35/0x88:02 RMW + fixed 0x140272d98 preset + final 0x88:14 RMW\n");
	seq_puts(s, "skipped_by_design: 0x88:f5 mask-table write is not applied because table index/value is not confirmed\n");
	seq_puts(s, "skipped_by_design: 1080p-like tail 0x15..0x1d is not applied here; it has separate stream-id/mode gates\n");
	seq_puts(s, "known_test_result: on this card, writes return completion=1 but immediate 0x1a reads still report zero; treat write success as command acceptance, not latched register proof\n");
	if (!hd->pipeline_ready) {
		seq_puts(s, "blocked; pipeline_ready is false, run the persistent init sequence first\n");
		return 0;
	}
	if (!allow_mailbox_writes || !allow_i2c_read_command1a ||
	    !allow_capture_88_writes) {
		seq_puts(s, "blocked; reload with allow_mailbox_writes=1 allow_i2c_read_command1a=1 allow_capture_88_writes=1 after pipeline-ready init\n");
		return 0;
	}

	mutex_lock(&hd->mailbox_lock);
	seq_puts(s, "before_reads:\n");
	ret = hd60pro_capture_88_dump_regs_locked(hd, s, "before");
	if (ret)
		goto out_unlock;

	ret = hd60pro_i2c_read8_locked(hd, s, "common_read_88_02", 0x88,
				       0x02, &value02);
	if (ret)
		goto out_unlock;
	value02 = (value02 & 0xfa) | 0x02;

	for (i = 0; i < ARRAY_SIZE(preset); i++) {
		char name[48];

		snprintf(name, sizeof(name), "write_88_%02x_min",
			 preset[i].reg);
		ret = hd60pro_i2c_write8_locked(hd, s, name, 0x88,
						preset[i].reg,
						preset[i].value);
		if (ret)
			goto out_unlock;
	}

	ret = hd60pro_i2c_write8_locked(hd, s, "common_write_88_02_rmw",
					0x88, 0x02, value02);
	if (ret)
		goto out_unlock;
	ret = hd60pro_i2c_read8_locked(hd, s, "final_read_88_14", 0x88,
				       0x14, &value14);
	if (ret)
		goto out_unlock;
	value14 &= 0x9f;
	ret = hd60pro_i2c_write8_locked(hd, s, "final_write_88_14_and9f",
					0x88, 0x14, value14);
	if (ret)
		goto out_unlock;

	seq_puts(s, "after_reads:\n");
	ret = hd60pro_capture_88_dump_regs_locked(hd, s, "after");

out_unlock:
	mutex_unlock(&hd->mailbox_lock);
	seq_printf(s, "result: %d\n", ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_apply_capture_88_preset_2400_min);

static int hd60pro_firmware_endpoint_tables_show(struct seq_file *s,
						 void *unused)
{
	unsigned int i;

	seq_puts(s, "firmware_endpoint_tables: extracted from embedded ep.ko .rodata\n");
	seq_puts(s, "source_symbols: ep_cmds_size ep_fw_update_size ep_ints_size ep_ints_1080p_size ep_aic_size\n");
	seq_puts(s, "disassembly_status: decoded with local Capstone/pyelftools helper; PC-literal relocations identify sysfs_notify targets\n");
	seq_puts(s, "ep_cmds_size_nonzero:\n");
	for (i = 0; i < ARRAY_SIZE(hd60pro_ep_cmds_size); i++) {
		if (hd60pro_ep_cmds_size[i])
			seq_printf(s, "  cmd_%02u_payload_bytes: 0x%02x\n",
				   i, hd60pro_ep_cmds_size[i]);
	}
	seq_puts(s, "ep_ints_size_nonzero:\n");
	for (i = 0; i < ARRAY_SIZE(hd60pro_ep_ints_size); i++) {
		if (hd60pro_ep_ints_size[i])
			seq_printf(s, "  int_%02u_payload_bytes: 0x%02x\n",
				   i, hd60pro_ep_ints_size[i]);
	}
	seq_puts(s, "ep_ints_1080p_size_nonzero:\n");
	for (i = 0; i < ARRAY_SIZE(hd60pro_ep_ints_1080p_size); i++) {
		if (hd60pro_ep_ints_1080p_size[i])
			seq_printf(s, "  int1080p_%02u_payload_bytes: 0x%02x\n",
				   i, hd60pro_ep_ints_1080p_size[i]);
	}
	seq_puts(s, "ep_aic_size_nonzero:\n");
	for (i = 0; i < ARRAY_SIZE(hd60pro_ep_aic_size); i++) {
		if (hd60pro_ep_aic_size[i])
			seq_printf(s, "  aic_%02u_payload_bytes: 0x%02x\n",
				   i, hd60pro_ep_aic_size[i]);
	}
	seq_puts(s, "ep_cmds_size_interpretation: sysfs /sys/vpl_pciep/command accepts IDs 0x18..0x22; firmware ISR also handles interrupt/event IDs including 0x29/0x2a/0x50..0x52\n");
	seq_puts(s, "pciep_isr_0x29: SET_VIC_PARAMS log path; payload bytes at +0x05 fps, +0x06 fw/mode, +0x08 width, +0x0a height, +0x22 interrupt_reduce; zero width/height sets no_signal\n");
	seq_puts(s, "pciep_isr_0x2a: skips when no_signal, updates interrupt-reduce state, notifies audio_ctrl and epint or epint_1080p\n");
	seq_puts(s, "pciep_isr_notify_targets: command status epint epint_1080p audio_ctrl\n");
	seq_puts(s, "command_store_decoded: rejects IDs outside 0x18..0x22; accepted payload is copied into ep_command and ep_command+0x28 is set to 1\n");
	seq_puts(s, "pcie_set_outbound_decoded: export used by firmware DMA modules; selects one of five outbound window fields and programs endpoint registers +0x50/+0x54/+0x58/+0x74/+0x7c/+0xd4\n");
	seq_puts(s, "next_reverse_target: find Windows host transport that injects endpoint event 0x29/0x2a or writes the backing ep_command shared-memory region\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_firmware_endpoint_tables);

static int hd60pro_firmware_info_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	const struct firmware *fw;
	int ret;

	seq_printf(s, "firmware_name: %s\n", firmware_name);
	ret = request_firmware(&fw, firmware_name, &hd->pdev->dev);
	if (ret) {
		seq_printf(s, "request_firmware_result: %d\n", ret);
		seq_puts(s, "available: 0\n");
		return 0;
	}

	seq_puts(s, "available: 1\n");
	seq_printf(s, "size: %zu\n", fw->size);
	seq_printf(s, "windows_mmio_mapping_hypothesis: device+0x108 maps first memory resource, expected BAR0\n");
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "bar0_len: %pa\n", &hd->bar0_len);
	seq_printf(s, "bar5_len: %pa\n", &hd->bar5_len);
	seq_printf(s, "windows_copy_offset: 0x%x\n", HD60PRO_FW_WINDOW_OFFSET);
	seq_printf(s, "required_window_bytes: %zu\n",
		   fw->size + HD60PRO_FW_WINDOW_OFFSET);
	seq_printf(s, "aperture_fits_bar0: %d\n",
		   fw->size + HD60PRO_FW_WINDOW_OFFSET <= hd->bar0_len);
	seq_printf(s, "aperture_fits_bar5: %d\n",
		   fw->size + HD60PRO_FW_WINDOW_OFFSET <= hd->bar5_len);
	seq_printf(s, "aperture_fits_selected_mailbox_bar: %d\n",
		   fw->size + HD60PRO_FW_WINDOW_OFFSET <=
		   hd60pro_mailbox_len(hd));
	if (fw->size >= 8)
		seq_printf(s, "magic: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			   fw->data[0], fw->data[1], fw->data[2], fw->data[3],
			   fw->data[4], fw->data[5], fw->data[6], fw->data[7]);
	seq_puts(s, "hardware_written: 0\n");
	release_firmware(fw);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_firmware_info);

static int hd60pro_firmware_load_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	const struct firmware *fw;
	void __iomem *base = hd60pro_mailbox_base(hd);
	resource_size_t len = hd60pro_mailbox_len(hd);
	u32 prepare_completion = 0;
	u32 commit_completion = 0;
	u32 status0;
	u32 status1;
	u32 status2;
	u32 prepare_packet[3];
	u32 commit_packet[3];
	u32 prepare_irq_delta = 0;
	u32 commit_irq_delta = 0;
	int ret;

	if (!allow_firmware_load) {
		seq_puts(s, "disabled; reload with allow_firmware_load=1 allow_mailbox_writes=1 to run Windows firmware download sequence\n");
		return 0;
	}

	if (!allow_unsafe_visible_fw_prepare) {
		seq_puts(s, "blocked: command 0x0b on cold hardware was observed to make PCI config and MMIO read 0xffffffff\n");
		seq_puts(s, "reload with allow_unsafe_visible_fw_prepare=1 only after the missing pre-init/base-firmware step is understood\n");
		return 0;
	}

	if (!allow_mailbox_writes) {
		seq_puts(s, "disabled; allow_mailbox_writes=1 is also required\n");
		return 0;
	}

	if (hd->irq < 0) {
		seq_puts(s, "disabled; request_irq_vector=1 is required because Windows sends firmware commands through the async IRQ path\n");
		return 0;
	}

	if (!base) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	ret = request_firmware(&fw, firmware_name, &hd->pdev->dev);
	if (ret) {
		seq_printf(s, "request_firmware_result: %d\n", ret);
		return 0;
	}

	seq_printf(s, "firmware_name: %s\n", firmware_name);
	seq_printf(s, "firmware_size: %zu\n", fw->size);
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_printf(s, "mailbox_len: %pa\n", &len);
	seq_printf(s, "copy_offset: 0x%x\n", HD60PRO_FW_WINDOW_OFFSET);

	if (fw->size + HD60PRO_FW_WINDOW_OFFSET > len) {
		seq_puts(s, "result: aperture_too_small\n");
		seq_printf(s, "required_window_bytes: %zu\n",
			   fw->size + HD60PRO_FW_WINDOW_OFFSET);
		release_firmware(fw);
		return 0;
	}

	prepare_packet[0] = HD60PRO_MBOX_DOORBELL;
	prepare_packet[1] = HD60PRO_MBOX_CMD_DOWNLOAD_FW_PREPARE;
	prepare_packet[2] = fw->size;
	commit_packet[0] = HD60PRO_MBOX_DOORBELL;
	commit_packet[1] = HD60PRO_MBOX_CMD_DOWNLOAD_FW_COMMIT;
	commit_packet[2] = 1;

	mutex_lock(&hd->mailbox_lock);
	ret = hd60pro_mailbox_send_async_locked(hd, prepare_packet,
						ARRAY_SIZE(prepare_packet),
						HD60PRO_MBOX_ASYNC_TIMEOUT_MS,
						&prepare_completion,
						&prepare_irq_delta);
	seq_printf(s, "prepare_command: 0x%08x 0x%08x 0x%08x\n",
		   prepare_packet[0], prepare_packet[1], prepare_packet[2]);
	seq_printf(s, "prepare_result: %d\n", ret);
	seq_printf(s, "prepare_completion: 0x%08x\n", prepare_completion);
	seq_printf(s, "prepare_irq_delta: %u\n", prepare_irq_delta);
	if (ret)
		goto out_unlock;

	memcpy_toio(base + HD60PRO_FW_WINDOW_OFFSET, fw->data, fw->size);
	wmb();
	seq_printf(s, "copied_bytes: %zu\n", fw->size);

	ret = hd60pro_mailbox_send_async_locked(hd, commit_packet,
						ARRAY_SIZE(commit_packet),
						HD60PRO_VISIBLE_FW_COMMIT_TIMEOUT_MS,
						&commit_completion,
						&commit_irq_delta);
	seq_printf(s, "commit_command: 0x%08x 0x%08x 0x%08x\n",
		   commit_packet[0], commit_packet[1], commit_packet[2]);
	seq_printf(s, "commit_result: %d\n", ret);
	seq_printf(s, "commit_completion: 0x%08x\n", commit_completion);
	seq_printf(s, "commit_irq_delta: %u\n", commit_irq_delta);
	msleep(100);

out_unlock:
	status0 = ioread32(base + 0x008);
	status1 = ioread32(base + 0x00c);
	status2 = ioread32(base + 0x010);
	mutex_unlock(&hd->mailbox_lock);

	seq_printf(s, "status_bar_%s_0x08: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status0);
	seq_printf(s, "status_bar_%s_0x0c: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status1);
	seq_printf(s, "status_bar_%s_0x10: 0x%08x\n",
		   hd60pro_mailbox_bar_name(), status2);
	seq_printf(s, "windows_success_condition_bar_%s_0x08_eq_0: %d\n",
		   hd60pro_mailbox_bar_name(), status0 == 0);
	seq_printf(s, "result: %d\n", ret);
	release_firmware(fw);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_firmware_load);

static void hd60pro_fill_pix_format(struct v4l2_pix_format *pix)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = HD60PRO_DEFAULT_WIDTH;
	pix->height = HD60PRO_DEFAULT_HEIGHT;
	pix->pixelformat = V4L2_PIX_FMT_YUYV;
	pix->field = HD60PRO_DEFAULT_FIELD;
	pix->bytesperline = HD60PRO_DEFAULT_WIDTH * 2;
	pix->sizeimage = pix->bytesperline * HD60PRO_DEFAULT_HEIGHT;
	pix->colorspace = HD60PRO_DEFAULT_COLORSPACE;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static unsigned int hd60pro_frame_size(void)
{
	struct v4l2_pix_format pix;

	hd60pro_fill_pix_format(&pix);
	return pix.sizeimage;
}

struct hd60pro_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

static void hd60pro_complete_synthetic_buffers(struct hd60pro_dev *hd);

static int hd60pro_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			       unsigned int *nplanes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	unsigned int size = hd60pro_frame_size();

	if (*nplanes) {
		if (*nplanes != 1 || sizes[0] < size)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = size;
	if (*nbuffers < 2)
		*nbuffers = 2;

	return 0;
}

static int hd60pro_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < hd60pro_frame_size())
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, hd60pro_frame_size());
	return 0;
}

static void hd60pro_buf_queue(struct vb2_buffer *vb)
{
	struct hd60pro_dev *hd = vb2_get_drv_priv(vb->vb2_queue);
	struct hd60pro_buffer *buf =
		container_of(to_vb2_v4l2_buffer(vb), struct hd60pro_buffer, vb);
	bool complete_now;
	unsigned long flags;

	spin_lock_irqsave(&hd->queued_lock, flags);
	list_add_tail(&buf->list, &hd->queued_bufs);
	complete_now = hd->streaming;
	spin_unlock_irqrestore(&hd->queued_lock, flags);

	if (complete_now && synthetic_v4l2)
		hd60pro_complete_synthetic_buffers(hd);
}

static void hd60pro_fill_synthetic_frame(void *vaddr, size_t size)
{
	u8 *p = vaddr;
	size_t i;

	if (!p)
		return;

	for (i = 0; i + 3 < size; i += 4) {
		p[i + 0] = 16;
		p[i + 1] = 128;
		p[i + 2] = 16;
		p[i + 3] = 128;
	}
}

static void hd60pro_complete_synthetic_buffers(struct hd60pro_dev *hd)
{
	unsigned int size = hd60pro_frame_size();
	unsigned long flags;

	spin_lock_irqsave(&hd->queued_lock, flags);
	while (!list_empty(&hd->queued_bufs) && hd->streaming) {
		struct hd60pro_buffer *buf =
			list_first_entry(&hd->queued_bufs, struct hd60pro_buffer,
					 list);
		u32 sequence = hd->sequence++;
		u64 timestamp = ktime_get_ns();
		void *vaddr;

		list_del(&buf->list);
		spin_unlock_irqrestore(&hd->queued_lock, flags);

		vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
		hd60pro_fill_synthetic_frame(vaddr, size);
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, size);
		buf->vb.sequence = sequence;
		buf->vb.field = HD60PRO_DEFAULT_FIELD;
		buf->vb.vb2_buf.timestamp = timestamp;
		hd->last_frame_meta.timestamp_ns = timestamp;
		hd->last_frame_meta.duration_ns = HD60PRO_DEFAULT_FRAME_PERIOD_NS;
		hd->last_frame_meta.payload_bytes = size;
		hd->last_frame_meta.flags = 0x00000100;
		hd->last_frame_meta.extra = synthetic_v4l2 ? 1 : 0;
		hd->last_frame_meta.sequence = sequence;
		hd->direct_memory_blob[0] = sequence;
		hd->direct_memory_blob[1] = size;
		hd->direct_memory_blob[2] = hd->last_frame_meta.flags;
		hd->direct_memory_blob[3] = hd->last_frame_meta.extra;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

		spin_lock_irqsave(&hd->queued_lock, flags);
	}
	spin_unlock_irqrestore(&hd->queued_lock, flags);
}

static void hd60pro_return_queued_buffers(struct hd60pro_dev *hd,
					  enum vb2_buffer_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&hd->queued_lock, flags);
	while (!list_empty(&hd->queued_bufs)) {
		struct hd60pro_buffer *buf =
			list_first_entry(&hd->queued_bufs, struct hd60pro_buffer,
					 list);

		list_del(&buf->list);
		spin_unlock_irqrestore(&hd->queued_lock, flags);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		spin_lock_irqsave(&hd->queued_lock, flags);
	}
	spin_unlock_irqrestore(&hd->queued_lock, flags);
}

static int hd60pro_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hd60pro_dev *hd = vb2_get_drv_priv(q);

	hd->streaming = true;
	if (!synthetic_v4l2) {
		hd60pro_return_queued_buffers(hd, VB2_BUF_STATE_ERROR);
		return 0;
	}

	/*
	 * DMA programming is not decoded yet. Complete queued buffers with a
	 * deterministic black YUYV frame so userspace can exercise the V4L2/vb2
	 * path while hardware capture bring-up continues independently.
	 */
	hd60pro_complete_synthetic_buffers(hd);

	return 0;
}

static void hd60pro_stop_streaming(struct vb2_queue *q)
{
	struct hd60pro_dev *hd = vb2_get_drv_priv(q);

	hd->streaming = false;
	hd60pro_return_queued_buffers(hd, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops hd60pro_vb2_ops = {
	.queue_setup = hd60pro_queue_setup,
	.buf_prepare = hd60pro_buf_prepare,
	.buf_queue = hd60pro_buf_queue,
	.start_streaming = hd60pro_start_streaming,
	.stop_streaming = hd60pro_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int hd60pro_vidioc_querycap(struct file *file, void *priv,
				   struct v4l2_capability *cap)
{
	struct hd60pro_dev *hd = video_drvdata(file);

	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, hd->board ? hd->board->name : "Elgato HD60 Pro",
		sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(hd->pdev));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
			   V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int hd60pro_vidioc_enum_input(struct file *file, void *priv,
				     struct v4l2_input *input)
{
	if (input->index)
		return -EINVAL;

	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	input->status = V4L2_IN_ST_NO_SIGNAL;

	return 0;
}

static int hd60pro_vidioc_g_input(struct file *file, void *priv,
				  unsigned int *input)
{
	*input = 0;
	return 0;
}

static int hd60pro_vidioc_s_input(struct file *file, void *priv,
				  unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int hd60pro_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
					   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	strscpy(f->description, "YUYV 4:2:2 (diagnostic placeholder)",
		sizeof(f->description));

	return 0;
}

static int hd60pro_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	hd60pro_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int hd60pro_vidioc_try_fmt_vid_cap(struct file *file, void *priv,
					  struct v4l2_format *f)
{
	if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
		f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

	hd60pro_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int hd60pro_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	return hd60pro_vidioc_try_fmt_vid_cap(file, priv, f);
}

static int hd60pro_vidioc_enum_framesizes(struct file *file, void *priv,
					  struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index || fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = HD60PRO_DEFAULT_WIDTH;
	fsize->discrete.height = HD60PRO_DEFAULT_HEIGHT;

	return 0;
}

static int hd60pro_vidioc_enum_frameintervals(struct file *file, void *priv,
					      struct v4l2_frmivalenum *fival)
{
	if (fival->index || fival->pixel_format != V4L2_PIX_FMT_YUYV ||
	    fival->width != HD60PRO_DEFAULT_WIDTH ||
	    fival->height != HD60PRO_DEFAULT_HEIGHT)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = 1;
	fival->discrete.denominator = 60;

	return 0;
}

static void hd60pro_fill_dv_timings(struct v4l2_dv_timings *timings)
{
	*timings = (struct v4l2_dv_timings) {
		.type = V4L2_DV_BT_656_1120,
		.bt = {
			.width = HD60PRO_DEFAULT_WIDTH,
			.height = HD60PRO_DEFAULT_HEIGHT,
			.interlaced = V4L2_DV_PROGRESSIVE,
			.pixelclock = HD60PRO_DEFAULT_PIXELCLOCK,
			.hfrontporch = 88,
			.hsync = 44,
			.hbackporch = 148,
			.vfrontporch = 4,
			.vsync = 5,
			.vbackporch = 36,
			.standards = V4L2_DV_BT_STD_CEA861,
			.flags = V4L2_DV_FL_CAN_REDUCE_FPS,
		},
	};
}

static int hd60pro_vidioc_query_dv_timings(struct file *file, void *priv,
					   struct v4l2_dv_timings *timings)
{
	hd60pro_fill_dv_timings(timings);
	return 0;
}

static int hd60pro_vidioc_g_dv_timings(struct file *file, void *priv,
				       struct v4l2_dv_timings *timings)
{
	hd60pro_fill_dv_timings(timings);
	return 0;
}

static int hd60pro_vidioc_s_dv_timings(struct file *file, void *priv,
				       struct v4l2_dv_timings *timings)
{
	struct v4l2_dv_timings expected;

	hd60pro_fill_dv_timings(&expected);
	if (timings->type != expected.type ||
	    timings->bt.width != expected.bt.width ||
	    timings->bt.height != expected.bt.height)
		return -EINVAL;

	*timings = expected;
	return 0;
}

static int hd60pro_vidioc_enum_dv_timings(struct file *file, void *priv,
					  struct v4l2_enum_dv_timings *timings)
{
	if (timings->index)
		return -EINVAL;

	hd60pro_fill_dv_timings(&timings->timings);
	return 0;
}

static int hd60pro_vidioc_streamon(struct file *file, void *priv,
				   enum v4l2_buf_type type)
{
	return vb2_ioctl_streamon(file, priv, type);
}

static int hd60pro_vidioc_streamoff(struct file *file, void *priv,
				    enum v4l2_buf_type type)
{
	return vb2_ioctl_streamoff(file, priv, type);
}

static const struct v4l2_file_operations hd60pro_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = v4l2_fh_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static const struct v4l2_ioctl_ops hd60pro_ioctl_ops = {
	.vidioc_querycap = hd60pro_vidioc_querycap,
	.vidioc_enum_input = hd60pro_vidioc_enum_input,
	.vidioc_g_input = hd60pro_vidioc_g_input,
	.vidioc_s_input = hd60pro_vidioc_s_input,
	.vidioc_enum_fmt_vid_cap = hd60pro_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = hd60pro_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = hd60pro_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = hd60pro_vidioc_s_fmt_vid_cap,
	.vidioc_enum_framesizes = hd60pro_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = hd60pro_vidioc_enum_frameintervals,
	.vidioc_query_dv_timings = hd60pro_vidioc_query_dv_timings,
	.vidioc_g_dv_timings = hd60pro_vidioc_g_dv_timings,
	.vidioc_s_dv_timings = hd60pro_vidioc_s_dv_timings,
	.vidioc_enum_dv_timings = hd60pro_vidioc_enum_dv_timings,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = hd60pro_vidioc_streamon,
	.vidioc_streamoff = hd60pro_vidioc_streamoff,
};

static int hd60pro_register_v4l2(struct hd60pro_dev *hd)
{
	struct video_device *vdev = &hd->vdev;
	struct vb2_queue *q = &hd->vb2q;
	int ret;

	if (!enable_v4l2)
		return 0;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF;
	q->drv_priv = hd;
	q->buf_struct_size = sizeof(struct hd60pro_buffer);
	q->ops = &hd60pro_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &hd->video_lock;
	q->dev = &hd->pdev->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return dev_err_probe(&hd->pdev->dev, ret,
				     "vb2_queue_init failed\n");

	ret = v4l2_device_register(&hd->pdev->dev, &hd->v4l2_dev);
	if (ret)
		return dev_err_probe(&hd->pdev->dev, ret,
				     "v4l2_device_register failed\n");

	strscpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->v4l2_dev = &hd->v4l2_dev;
	vdev->fops = &hd60pro_fops;
	vdev->ioctl_ops = &hd60pro_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->queue = q;
	vdev->lock = &hd->video_lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
			    V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT;
	video_set_drvdata(vdev, hd);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_device_unregister(&hd->v4l2_dev);
		return dev_err_probe(&hd->pdev->dev, ret,
				     "video_register_device failed\n");
	}

	dev_info(&hd->pdev->dev, "registered diagnostic V4L2 node /dev/video%d\n",
		 vdev->num);
	return 0;
}

static void hd60pro_unregister_v4l2(struct hd60pro_dev *hd)
{
	if (!enable_v4l2)
		return;

	video_unregister_device(&hd->vdev);
	v4l2_device_unregister(&hd->v4l2_dev);
}

static int hd60pro_info_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	struct pci_dev *pdev = hd->pdev;
	u16 command;
	u16 status;
	u8 revision;
	resource_size_t bar0_start;
	resource_size_t bar5_start;
	int pm_cap;
	int pcie_cap;
	int msi_cap;
	int msix_cap;

	pci_read_config_word(pdev, PCI_COMMAND, &command);
	pci_read_config_word(pdev, PCI_STATUS, &status);
	pci_read_config_byte(pdev, PCI_REVISION_ID, &revision);
	bar0_start = pci_resource_start(pdev, HD60PRO_BAR0);
	bar5_start = pci_resource_start(pdev, HD60PRO_BAR5);

	pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM);
	pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);

	seq_printf(s, "driver: %s\n", KBUILD_MODNAME);
	seq_printf(s, "board: %s\n", hd->board ? hd->board->name : "unknown");
	seq_printf(s, "pci_name: %s\n", pci_name(pdev));
	seq_printf(s, "vendor_device: %04x:%04x\n", pdev->vendor, pdev->device);
	seq_printf(s, "subsystem: %04x:%04x\n",
		   pdev->subsystem_vendor, pdev->subsystem_device);
	seq_printf(s, "class: 0x%06x\n", pdev->class >> 8);
	seq_printf(s, "revision: 0x%02x\n", revision);
	seq_printf(s, "command: 0x%04x\n", command);
	seq_printf(s, "status: 0x%04x\n", status);
	seq_printf(s, "irq: %d\n", hd->irq);
	seq_printf(s, "irq_count: %u\n", hd->irq_count);
	seq_printf(s, "mailbox_irq_count: %u\n", hd->mailbox_irq_count);
	seq_printf(s, "last_irq_status: 0x%08x\n", hd->last_irq_status);
	seq_printf(s, "pipeline_ready: %d\n", hd->pipeline_ready);
	seq_printf(s, "streaming: %d\n", hd->streaming);
	seq_printf(s, "v4l2_synthetic_frames: %d\n", synthetic_v4l2);
	seq_printf(s, "busmaster_requested: %d\n", enable_busmaster);
	seq_printf(s, "irq_requested: %d\n", request_irq_vector);
	seq_printf(s, "v4l2_enabled: %d\n", enable_v4l2);
	seq_printf(s, "mailbox_writes_allowed: %d\n", allow_mailbox_writes);
	seq_printf(s, "firmware_load_allowed: %d\n", allow_firmware_load);
	seq_printf(s, "preinit_command1_allowed: %d\n", allow_preinit_command1);
	seq_printf(s, "fw_status_command10_allowed: %d\n",
		   allow_fw_status_command10);
	seq_printf(s, "gpio_command17_allowed: %d\n", allow_gpio_command17);
	seq_printf(s, "gpio17_sequence_allowed: %d\n", allow_gpio17_sequence);
	seq_printf(s, "i2c_read_command1a_allowed: %d\n",
		   allow_i2c_read_command1a);
	seq_printf(s, "logo_upload_allowed: %d\n", allow_logo_upload);
	seq_printf(s, "post_logo_pipeline_allowed: %d\n",
		   allow_post_logo_pipeline);
	seq_printf(s, "cmd1d_write_allowed: %d\n", allow_cmd1d_write);
	seq_printf(s, "auto_init_requested: %d\n", auto_init);
	seq_puts(s, "auto_init_status: not wired; use scripts/load-initialized-root.sh for the persistent reconstructed sequence\n");
	seq_printf(s, "prepare_dma_buffers: %d\n", prepare_dma_buffers);
	seq_printf(s, "synthetic_v4l2: %d\n", synthetic_v4l2);
	seq_printf(s, "unsafe_visible_fw_prepare_allowed: %d\n",
		   allow_unsafe_visible_fw_prepare);
	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_puts(s, "windows_mapping_note: device+0x108 is first mapped memory resource; device+0x110 is second\n");
	if (enable_v4l2)
		seq_printf(s, "video_device: /dev/video%d\n", hd->vdev.num);
	seq_printf(s, "mmio_dump_enabled: %d\n", mmio_dump);
	seq_printf(s, "bar0_start: %pa\n", &bar0_start);
	seq_printf(s, "bar0_len: %pa\n", &hd->bar0_len);
	seq_printf(s, "bar5_start: %pa\n", &bar5_start);
	seq_printf(s, "bar5_len: %pa\n", &hd->bar5_len);
	seq_printf(s, "cap_pm: 0x%x\n", pm_cap);
	seq_printf(s, "cap_pcie: 0x%x\n", pcie_cap);
	seq_printf(s, "cap_msi: 0x%x\n", msi_cap);
	seq_printf(s, "cap_msix: 0x%x\n", msix_cap);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_info);

static int hd60pro_pci_config_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	unsigned int offset;

	for (offset = 0; offset < HD60PRO_CONFIG_DUMP_BYTES; offset += 16) {
		u8 b[16];
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(b); i++)
			pci_read_config_byte(hd->pdev, offset + i, &b[i]);

		seq_printf(s,
			   "%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			   offset, b[0], b[1], b[2], b[3], b[4], b[5],
			   b[6], b[7], b[8], b[9], b[10], b[11], b[12],
			   b[13], b[14], b[15]);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_pci_config);

static bool hd60pro_mmio_window_dead(void __iomem *base)
{
	return ioread32(base + 0x000) == U32_MAX &&
	       ioread32(base + 0x02c) == U32_MAX &&
	       ioread32(base + 0x030) == U32_MAX &&
	       ioread32(base + 0x050) == U32_MAX;
}

static void hd60pro_dump_bar(struct seq_file *s, void __iomem *base,
			     resource_size_t len, unsigned int requested_len)
{
	u8 __iomem *bytes = base;
	unsigned int offset;
	unsigned int limit = min_t(resource_size_t, len, requested_len);

	if (!mmio_dump) {
		seq_puts(s, "disabled; reload with mmio_dump=1 for read-only diagnostics\n");
		return;
	}

	if (hd60pro_mmio_window_dead(base)) {
		seq_puts(s, "device inaccessible; MMIO reads return 0xffffffff\n");
		return;
	}

	for (offset = 0; offset < limit; offset += 16) {
		u32 v0 = ioread32(bytes + offset + 0);
		u32 v1 = ioread32(bytes + offset + 4);
		u32 v2 = ioread32(bytes + offset + 8);
		u32 v3 = ioread32(bytes + offset + 12);

		seq_printf(s, "%04x: %08x %08x %08x %08x\n",
			   offset, v0, v1, v2, v3);
	}
}

static int hd60pro_bar0_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;

	hd60pro_dump_bar(s, hd->bar0, hd->bar0_len, HD60PRO_DUMP_BYTES);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_bar0);

static int hd60pro_bar5_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;

	hd60pro_dump_bar(s, hd->bar5, hd->bar5_len, HD60PRO_DUMP_BYTES);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_bar5);

static int hd60pro_bar5_full_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;

	hd60pro_dump_bar(s, hd->bar5, hd->bar5_len, hd->bar5_len);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_bar5_full);

static const char *hd60pro_mailbox_reg_name(unsigned int offset)
{
	switch (offset) {
	case 0x000:
		return "doorbell";
	case 0x004:
		return "arg0_command";
	case 0x008:
		return "arg1_or_result0";
	case 0x00c:
		return "arg2_or_result1";
	case 0x010:
		return "arg3_or_result2";
	case 0x014:
		return "arg4";
	case 0x02c:
		return "completion_bit0";
	case 0x030:
		return "irq_status_or_scratch";
	case 0x050:
		return "queue_state_or_scratch";
	default:
		return NULL;
	}
}

static int hd60pro_mailbox_regs_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u8 __iomem *bytes = hd60pro_mailbox_base(hd);
	static const unsigned int offsets[] = {
		0x000, 0x004, 0x008, 0x00c, 0x010, 0x014,
		0x02c, 0x030, 0x050,
	};
	unsigned int i;

	if (!mmio_dump) {
		seq_puts(s, "disabled; reload with mmio_dump=1 for read-only diagnostics\n");
		return 0;
	}

	if (!bytes) {
		seq_puts(s, "selected mailbox BAR is not mapped\n");
		return 0;
	}

	seq_printf(s, "mailbox_bar: %s\n", hd60pro_mailbox_bar_name());
	seq_puts(s, "offset value name\n");
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		unsigned int offset = offsets[i];
		u32 value = ioread32(bytes + offset);
		const char *name = hd60pro_mailbox_reg_name(offset);

		seq_printf(s, "0x%03x 0x%08x %s\n", offset, value,
			   name ? name : "-");
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_mailbox_regs);

static const char *hd60pro_bar5_reg_name(unsigned int offset)
{
	switch (offset) {
	case 0x000:
		return "sideband_status_or_mirror";
	case 0x004:
		return "sideband_arg0_or_status";
	case 0x008:
		return "sideband_arg1_or_status";
	case 0x00c:
		return "sideband_arg2_or_status";
	case 0x010:
		return "sideband_arg3_or_status";
	case 0x014:
		return "sideband_arg4_or_status";
	case 0x018:
		return "sideband_or_irq_status";
	case HD60PRO_REG_MBOX_COMPLETE:
		return "sideband_completion_or_status";
	case 0x030:
		return "irq_status_or_bar0_ref_a";
	case 0x038:
		return "bar0_ref_b";
	case 0x040:
		return "irq_payload0_windows_dpc";
	case 0x044:
		return "irq_payload1_windows_dpc";
	case 0x048:
		return "irq_payload2_windows_dpc";
	case 0x04c:
		return "irq_payload3_windows_dpc";
	case 0x050:
		return "mailbox_or_irq_queue_state";
	case 0x0d4:
		return "mailbox_or_firmware_state";
	case HD60PRO_REG_IRQ_ACK_SIDEBAND:
		return "irq_ack_sideband_windows";
	default:
		return NULL;
	}
}

static int hd60pro_bar5_regs_show(struct seq_file *s, void *unused)
{
	struct hd60pro_dev *hd = s->private;
	u8 __iomem *bytes = hd->bar5;
	unsigned int offset;
	unsigned int limit = min_t(resource_size_t, hd->bar5_len,
				   HD60PRO_BAR5_REAL_BYTES);

	if (!mmio_dump) {
		seq_puts(s, "disabled; reload with mmio_dump=1 for read-only diagnostics\n");
		return 0;
	}

	if (hd60pro_mmio_window_dead(bytes)) {
		seq_puts(s, "device inaccessible; BAR5 reads return 0xffffffff\n");
		return 0;
	}

	seq_printf(s, "decoded_window_bytes: 0x%x\n", limit);
	seq_puts(s, "offset value name\n");

	for (offset = 0; offset < limit; offset += 4) {
		u32 value = ioread32(bytes + offset);
		const char *name = hd60pro_bar5_reg_name(offset);

		if (!name && (value == 0x00000000 || value == 0x11000001))
			continue;

		seq_printf(s, "0x%03x 0x%08x %s\n", offset, value,
			   name ? name : "-");
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60pro_bar5_regs);

static void hd60pro_debugfs_init(struct hd60pro_dev *hd)
{
	char name[32];

	snprintf(name, sizeof(name), "%s", pci_name(hd->pdev));
	hd->debugfs_dir = debugfs_create_dir(name, hd60pro_debugfs_root);
	debugfs_create_file("info", 0400, hd->debugfs_dir, hd,
			    &hd60pro_info_fops);
	debugfs_create_file("pci_config", 0400, hd->debugfs_dir, hd,
			    &hd60pro_pci_config_fops);
	debugfs_create_file("health", 0400, hd->debugfs_dir, hd,
			    &hd60pro_health_fops);
	debugfs_create_file("firmware_info", 0400, hd->debugfs_dir, hd,
			    &hd60pro_firmware_info_fops);
	debugfs_create_file("dma_info", 0400, hd->debugfs_dir, hd,
			    &hd60pro_dma_info_fops);
	debugfs_create_file("capture_info", 0400, hd->debugfs_dir, hd,
			    &hd60pro_capture_info_fops);
	debugfs_create_file("endpoint_info", 0400, hd->debugfs_dir, hd,
			    &hd60pro_endpoint_info_fops);
	debugfs_create_file("direct_memory_info", 0400, hd->debugfs_dir, hd,
			    &hd60pro_direct_memory_info_fops);
	debugfs_create_file("windows_directmemory_drain_path", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_directmemory_drain_path_fops);
	debugfs_create_file("windows_contiguous_buffer_layout", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_contiguous_buffer_layout_fops);
	debugfs_create_file("capture_start_plan", 0400, hd->debugfs_dir, hd,
			    &hd60pro_capture_start_plan_fops);
	debugfs_create_file("endpoint_command_plan", 0400, hd->debugfs_dir, hd,
			    &hd60pro_endpoint_command_plan_fops);
	debugfs_create_file("set_vic_event_record", 0400, hd->debugfs_dir, hd,
			    &hd60pro_set_vic_event_record_fops);
	debugfs_create_file("endpoint_transport_plan", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_endpoint_transport_plan_fops);
	debugfs_create_file("windows_payload_uploader", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_payload_uploader_fops);
	debugfs_create_file("windows_stream_state_flow", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_stream_state_flow_fops);
	debugfs_create_file("windows_stream_consumers", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_stream_consumers_fops);
	debugfs_create_file("windows_frame_counter_info", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_frame_counter_info_fops);
	debugfs_create_file("windows_buffer_queue_info", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_buffer_queue_info_fops);
	debugfs_create_file("windows_frame_producer_search", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_frame_producer_search_fops);
	debugfs_create_file("windows_external_buffer_info", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_external_buffer_info_fops);
	debugfs_create_file("windows_dma_mapping_info", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_dma_mapping_info_fops);
	debugfs_create_file("windows_dma_publish_search", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_dma_publish_search_fops);
	debugfs_create_file("windows_bar_mapping_xrefs", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_bar_mapping_xrefs_fops);
	debugfs_create_file("windows_worker_event_path", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_worker_event_path_fops);
	debugfs_create_file("windows_event_queue_xrefs", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_event_queue_xrefs_fops);
	debugfs_create_file("windows_event_callback_bridge", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_event_callback_bridge_fops);
	debugfs_create_file("windows_stream_callback_search", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_stream_callback_search_fops);
	debugfs_create_file("windows_physical_buffer_xrefs", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_physical_buffer_xrefs_fops);
	debugfs_create_file("firmware_userland_flow", 0400, hd->debugfs_dir,
			    hd, &hd60pro_firmware_userland_flow_fops);
	debugfs_create_file("endpoint_bridge_regs", 0400, hd->debugfs_dir, hd,
			    &hd60pro_endpoint_bridge_regs_fops);
	debugfs_create_file("firmware_pcie_outbound_regs", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_firmware_pcie_outbound_regs_fops);
	debugfs_create_file("firmware_dmac_outbound_path", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_firmware_dmac_outbound_path_fops);
	debugfs_create_file("windows_88_update_plan", 0400, hd->debugfs_dir,
			    hd, &hd60pro_windows_88_update_plan_fops);
	debugfs_create_file("windows_calibration_info", 0400, hd->debugfs_dir,
			    hd, &hd60pro_windows_calibration_info_fops);
	debugfs_create_file("windows_bridge_attach_info", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_bridge_attach_info_fops);
	debugfs_create_file("windows_capture_88_presets", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_capture_88_presets_fops);
	debugfs_create_file("windows_88_mask_tables", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_windows_88_mask_tables_fops);
	debugfs_create_file("apply_capture_88_preset_2400_min", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_apply_capture_88_preset_2400_min_fops);
	debugfs_create_file("firmware_endpoint_tables", 0400, hd->debugfs_dir,
			    hd, &hd60pro_firmware_endpoint_tables_fops);
	debugfs_create_file("firmware_load", 0400, hd->debugfs_dir, hd,
			    &hd60pro_firmware_load_fops);
	debugfs_create_file("preinit_command1", 0400, hd->debugfs_dir, hd,
			    &hd60pro_preinit_command1_fops);
	debugfs_create_file("fw_status_command10", 0400, hd->debugfs_dir, hd,
			    &hd60pro_fw_status_command10_fops);
	debugfs_create_file("windows_init_plan", 0400, hd->debugfs_dir, hd,
			    &hd60pro_windows_init_plan_fops);
	debugfs_create_file("gpio17_line0", 0400, hd->debugfs_dir, hd,
			    &hd60pro_gpio17_line0_fops);
	debugfs_create_file("gpio17_generic_sequence", 0400, hd->debugfs_dir,
			    hd, &hd60pro_gpio17_generic_sequence_fops);
	debugfs_create_file("i2c1a_c0_dc_db", 0400, hd->debugfs_dir, hd,
			    &hd60pro_i2c1a_c0_dc_db_fops);
	debugfs_create_file("logo_upload_plan", 0400, hd->debugfs_dir, hd,
			    &hd60pro_logo_upload_plan_fops);
	debugfs_create_file("logo_upload_selector100", 0400, hd->debugfs_dir,
			    hd, &hd60pro_logo_upload_selector100_fops);
	debugfs_create_file("logo_upload_all", 0400, hd->debugfs_dir, hd,
			    &hd60pro_logo_upload_all_fops);
	debugfs_create_file("pre_28548c_logo_uploads_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_pre_28548c_logo_uploads_local_fops);
	debugfs_create_file("post_logo_plan", 0400, hd->debugfs_dir, hd,
			    &hd60pro_post_logo_plan_fops);
	debugfs_create_file("post_logo_pipeline_28548c_min", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_28548c_min_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_head_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_head_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table1_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table1_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table2_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table2_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table3_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table3_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table4_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table4_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table5_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table5_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table6_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table6_local_fops);
	debugfs_create_file("post_logo_pipeline_24dc28_table7_local_default",
			    0400, hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24dc28_table7_local_default_fops);
	debugfs_create_file("post_logo_pipeline_28548c_after_24dc28_tail",
			    0400, hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_28548c_after_24dc28_tail_fops);
	debugfs_create_file("post_logo_pipeline_286734_local_noop", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_286734_local_noop_fops);
	debugfs_create_file("post_logo_pipeline_28548c_local_prefix", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_28548c_local_prefix_fops);
	debugfs_create_file("post_logo_shadow_probe", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_shadow_probe_fops);
	debugfs_create_file("post_logo_pipeline_287224_min", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_287224_min_fops);
	debugfs_create_file("post_logo_pipeline_24c894_coeffs", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_24c894_coeffs_fops);
	debugfs_create_file("post_logo_pipeline_28548c_tail_local", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_pipeline_28548c_tail_local_fops);
	debugfs_create_file("post_logo_selector_shadow_probe", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_selector_shadow_probe_fops);
	debugfs_create_file("post_logo_cmd1d_a2", 0400, hd->debugfs_dir, hd,
			    &hd60pro_post_logo_cmd1d_a2_fops);
	debugfs_create_file("post_logo_cmd1d_variant_sweep", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_cmd1d_variant_sweep_fops);
	debugfs_create_file("post_logo_challenge_a2", 0400, hd->debugfs_dir,
			    hd, &hd60pro_post_logo_challenge_a2_fops);
	debugfs_create_file("post_logo_challenge_wait_a2", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_challenge_wait_a2_fops);
	debugfs_create_file("post_logo_challenge_sweep_a2", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_challenge_sweep_a2_fops);
	debugfs_create_file("post_logo_selector1c_dump", 0400,
			    hd->debugfs_dir, hd,
			    &hd60pro_post_logo_selector1c_dump_fops);
	debugfs_create_file("bar0_head", 0400, hd->debugfs_dir, hd,
			    &hd60pro_bar0_fops);
	debugfs_create_file("bar5_head", 0400, hd->debugfs_dir, hd,
			    &hd60pro_bar5_fops);
	debugfs_create_file("bar5_regs", 0400, hd->debugfs_dir, hd,
			    &hd60pro_bar5_regs_fops);
	debugfs_create_file("mailbox_regs", 0400, hd->debugfs_dir, hd,
			    &hd60pro_mailbox_regs_fops);
	debugfs_create_file("bar5_full", 0400, hd->debugfs_dir, hd,
			    &hd60pro_bar5_full_fops);
	debugfs_create_file("fw_version", 0400, hd->debugfs_dir, hd,
			    &hd60pro_fw_version_fops);
}

static int hd60pro_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct hd60pro_dev *hd;
	resource_size_t bar0_start;
	resource_size_t bar5_start;
	u16 command;
	u16 status;
	int ret;

	hd = devm_kzalloc(&pdev->dev, sizeof(*hd), GFP_KERNEL);
	if (!hd)
		return -ENOMEM;

	hd->pdev = pdev;
	hd->board = (const struct hd60pro_board *)id->driver_data;
	hd->irq = -1;
	mutex_init(&hd->mailbox_lock);
	mutex_init(&hd->video_lock);
	spin_lock_init(&hd->queued_lock);
	INIT_LIST_HEAD(&hd->queued_bufs);
	pci_set_drvdata(pdev, hd);

	if (mailbox_bar != HD60PRO_BAR0 && mailbox_bar != HD60PRO_BAR5)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "mailbox_bar must be 0 or 5\n");

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "pcim_enable_device failed\n");

	pci_read_config_word(pdev, PCI_COMMAND, &command);
	pci_read_config_word(pdev, PCI_STATUS, &status);
	if (command == U16_MAX || status == U16_MAX)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "PCI config space is inaccessible\n");

	ret = pcim_iomap_regions(pdev, BIT(HD60PRO_BAR0) | BIT(HD60PRO_BAR5),
				 KBUILD_MODNAME);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "BAR request/map failed\n");

	hd->bar0 = pcim_iomap_table(pdev)[HD60PRO_BAR0];
	hd->bar5 = pcim_iomap_table(pdev)[HD60PRO_BAR5];
	hd->bar0_len = pci_resource_len(pdev, HD60PRO_BAR0);
	hd->bar5_len = pci_resource_len(pdev, HD60PRO_BAR5);
	bar0_start = pci_resource_start(pdev, HD60PRO_BAR0);
	bar5_start = pci_resource_start(pdev, HD60PRO_BAR5);

	if (hd60pro_mmio_window_dead(hd->bar5))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "BAR5 MMIO window is inaccessible\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "DMA mask setup failed\n");
	}

	if (enable_busmaster)
		pci_set_master(pdev);

	if (request_irq_vector) {
		ret = pci_alloc_irq_vectors(pdev, 1, 1,
					    PCI_IRQ_MSI | PCI_IRQ_MSIX | PCI_IRQ_INTX);
		if (ret < 0)
			return dev_err_probe(&pdev->dev, ret, "IRQ vector allocation failed\n");

		hd->irq = pci_irq_vector(pdev, 0);
		ret = request_irq(hd->irq, hd60pro_irq, IRQF_SHARED,
				  KBUILD_MODNAME, hd);
		if (ret) {
			pci_free_irq_vectors(pdev);
			hd->irq = -1;
			return dev_err_probe(&pdev->dev, ret, "request_irq failed\n");
		}
	}

	if (prepare_dma_buffers) {
		ret = hd60pro_alloc_diag_dma(hd);
		if (ret) {
			if (hd->irq >= 0) {
				free_irq(hd->irq, hd);
				pci_free_irq_vectors(pdev);
				hd->irq = -1;
			}
			return dev_err_probe(&pdev->dev, ret,
					     "diagnostic DMA allocation failed\n");
		}
	}

	hd60pro_debugfs_init(hd);

	ret = hd60pro_register_v4l2(hd);
	if (ret) {
		debugfs_remove_recursive(hd->debugfs_dir);
		hd60pro_free_diag_dma(hd);
		if (hd->irq >= 0) {
			free_irq(hd->irq, hd);
			pci_free_irq_vectors(pdev);
			hd->irq = -1;
		}
		return ret;
	}

	dev_info(&pdev->dev,
		 "bound %s: bar0=%pa len=%pa bar5=%pa len=%pa busmaster=%d irq=%d mmio_dump=%d\n",
		 hd->board ? hd->board->name : "HD60 Pro diagnostics",
		 &bar0_start, &hd->bar0_len, &bar5_start, &hd->bar5_len,
		 enable_busmaster, hd->irq, mmio_dump);

	return 0;
}

static void hd60pro_remove(struct pci_dev *pdev)
{
	struct hd60pro_dev *hd = pci_get_drvdata(pdev);

	hd60pro_unregister_v4l2(hd);
	debugfs_remove_recursive(hd->debugfs_dir);
	hd60pro_free_diag_dma(hd);

	if (hd->irq >= 0) {
		free_irq(hd->irq, hd);
		pci_free_irq_vectors(pdev);
	}

	if (enable_busmaster)
		pci_clear_master(pdev);

	dev_info(&pdev->dev, "unbound HD60 Pro diagnostics\n");
}

static struct pci_driver hd60pro_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = hd60pro_pci_ids,
	.probe = hd60pro_probe,
	.remove = hd60pro_remove,
};

static int __init hd60pro_init(void)
{
	hd60pro_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
	return pci_register_driver(&hd60pro_pci_driver);
}

static void __exit hd60pro_exit(void)
{
	pci_unregister_driver(&hd60pro_pci_driver);
	debugfs_remove_recursive(hd60pro_debugfs_root);
}

module_init(hd60pro_init);
module_exit(hd60pro_exit);

MODULE_DESCRIPTION("Experimental Elgato HD60 Pro PCIe diagnostics driver");
MODULE_AUTHOR("Codex");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc");
