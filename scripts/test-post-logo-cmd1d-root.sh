#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

run_quiet()
{
	label="$1"
	seconds="$2"
	path="$3"
	out="$TMPDIR/$label.txt"

	if ! timeout "$seconds"s cat "$path" >"$out" 2>&1; then
		echo "== $label FAILED =="
		cat "$out"
		exit 1
	fi

	if grep -Eq '^(blocked|disabled|aborted|result: -|selected mailbox BAR is not mapped|device inaccessible)' "$out"; then
		echo "== $label BLOCKED =="
		cat "$out"
		exit 1
	fi
}

run_show()
{
	label="$1"
	seconds="$2"
	path="$3"

	echo
	echo "== $label =="
	timeout "$seconds"s cat "$path"
}

run_require_grep()
{
	label="$1"
	seconds="$2"
	path="$3"
	pattern="$4"
	out="$TMPDIR/$label.txt"

	if ! timeout "$seconds"s cat "$path" >"$out" 2>&1; then
		echo "== $label FAILED =="
		cat "$out"
		exit 1
	fi

	if ! grep -Eq "$pattern" "$out"; then
		echo "== $label UNEXPECTED =="
		cat "$out"
		exit 1
	fi
}

post_logo_pipeline_started()
{
	out="$TMPDIR/mailbox_regs.txt"

	if ! timeout 5s cat "$DBG/mailbox_regs" >"$out" 2>&1; then
		return 1
	fi

	grep -q '^0x004 0x0000001b ' "$out" &&
		grep -q '^0x02c 0x00000001 ' "$out" &&
		grep -q '^0x00c 0x00000018 ' "$out"
}

if ! ./scripts/install-logo-payloads-root.sh >"$TMPDIR/install-logo-payloads.txt" 2>&1; then
	echo "== install-logo-payloads-root FAILED =="
	cat "$TMPDIR/install-logo-payloads.txt"
	exit 1
fi

rmmod hd60prodrv 2>/dev/null || true
if ! ./scripts/load-safe.sh ./hd60prodrv.ko \
	mmio_dump=1 \
	allow_mailbox_writes=1 \
	allow_preinit_command1=1 \
	allow_fw_status_command10=1 \
	allow_gpio17_sequence=1 \
	allow_i2c_read_command1a=1 \
	allow_logo_upload=1 \
	allow_post_logo_pipeline=1 \
	allow_cmd1d_write=1 \
	request_irq_vector=1 \
	enable_v4l2=1 \
	prepare_dma_buffers=1 \
	allow_dma_capture=1 \
	force_32bit_dma=1 \
	allow_capture_88_writes=1 \
	synthetic_v4l2=0 \
	enable_busmaster=1 \
	mailbox_bar=0 >"$TMPDIR/load-safe.txt" 2>&1; then
	echo "== load-safe FAILED =="
	cat "$TMPDIR/load-safe.txt"
	exit 1
fi

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"

if ! post_logo_pipeline_started; then
	run_quiet preinit_command1 10 "$DBG/preinit_command1"
	run_quiet fw_status_command10 10 "$DBG/fw_status_command10"
	run_quiet gpio17_generic_sequence 15 "$DBG/gpio17_generic_sequence"
	run_quiet i2c1a_c0_dc_db 10 "$DBG/i2c1a_c0_dc_db"
	run_quiet logo_upload_all 40 "$DBG/logo_upload_all"
	run_quiet pre_28548c_logo_uploads_local 40 "$DBG/pre_28548c_logo_uploads_local"
	run_quiet post_logo_pipeline_28548c_min 10 "$DBG/post_logo_pipeline_28548c_min"
fi

run_quiet post_logo_pipeline_24dc28_head_local 10 "$DBG/post_logo_pipeline_24dc28_head_local"
run_quiet post_logo_pipeline_24dc28_table1_local 10 "$DBG/post_logo_pipeline_24dc28_table1_local"
run_quiet post_logo_pipeline_24dc28_table2_local 10 "$DBG/post_logo_pipeline_24dc28_table2_local"

run_quiet post_logo_pipeline_24dc28_table3_local 10 "$DBG/post_logo_pipeline_24dc28_table3_local"
run_quiet post_logo_pipeline_24dc28_table4_local 10 "$DBG/post_logo_pipeline_24dc28_table4_local"
run_quiet post_logo_pipeline_24dc28_table5_local 10 "$DBG/post_logo_pipeline_24dc28_table5_local"
run_quiet post_logo_pipeline_24dc28_table6_local 10 "$DBG/post_logo_pipeline_24dc28_table6_local"
run_quiet post_logo_pipeline_24dc28_table7_local_default 10 "$DBG/post_logo_pipeline_24dc28_table7_local_default"
run_quiet post_logo_pipeline_28548c_after_24dc28_tail 10 "$DBG/post_logo_pipeline_28548c_after_24dc28_tail"
run_quiet post_logo_pipeline_286734_local_noop 10 "$DBG/post_logo_pipeline_286734_local_noop"
run_quiet post_logo_shadow_probe 10 "$DBG/post_logo_shadow_probe"
run_quiet post_logo_pipeline_287224_min 10 "$DBG/post_logo_pipeline_287224_min"
run_quiet post_logo_pipeline_24c894_coeffs 10 "$DBG/post_logo_pipeline_24c894_coeffs"
run_quiet post_logo_pipeline_28548c_tail_local 10 "$DBG/post_logo_pipeline_28548c_tail_local"
run_quiet post_logo_pipeline_286734_local_noop 10 "$DBG/post_logo_pipeline_286734_local_noop"
run_quiet post_logo_pipeline_287224_min 10 "$DBG/post_logo_pipeline_287224_min"
run_quiet post_logo_pipeline_24c894_coeffs 10 "$DBG/post_logo_pipeline_24c894_coeffs"
run_quiet post_logo_selector_shadow_probe 10 "$DBG/post_logo_selector_shadow_probe"
run_quiet post_logo_cmd1d_a2 10 "$DBG/post_logo_cmd1d_a2"
run_require_grep post_logo_challenge_a2 10 "$DBG/post_logo_challenge_a2" '^pipeline_ready: 1$'

# Physical MST3367 at 0x98 (7-bit 0x4C) — initialized by FPGA ARM before driver.
# Expect: reg[0x55]=0x7D (CDR locked), reg[0x56]=0xFE, reg[0x5E]=0x9A.
# DO NOT run mst3367_hw_init here — GPIO8 pulse would reset the physical chip
# and force the FPGA ARM to reinitialise it (takes 60+ s, blocks capture).
# The FPGA ARM already has the chip in the correct state.
run_show mst3367_phys_test 10 "$DBG/mst3367_phys_test"

run_show gpio_read 5 "$DBG/gpio_read"

# Show BAR5 DMA window config (was run_quiet; now visible so we can see if
# BAR5+0x054 sticks or is read-only from the host side).
run_show bar5_dma_program 10 "$DBG/bar5_dma_program"

run_show capture_info 10 "$DBG/capture_info"
run_show health 10 "$DBG/health"

# Confirm physical CDR is still locked right before starting capture.
# If reg[0x55] is NOT 0x7D here, the chip was reset and is not ready.
run_show mst3367_phys_test 10 "$DBG/mst3367_phys_test"

# Full end-to-end stream test: sends cmds 0x29+0x2a+0x02+0x06 and polls 5 s
# for DMA frame IRQs, then peeks at the frame buffer.
# This tells us whether the firmware/DMA path is alive WITHOUT needing VLC.
run_show stream_start_test 120 "$DBG/stream_start_test"

echo
echo "=== After stream_start_test: check dmesg for start_streaming logs ==="
echo "    sudo dmesg | grep hd60pro | tail -20"
echo "=== If DMA frames received above, start VLC as root: ==="
echo "    sudo vlc v4l2:///dev/video1"
echo "=== Or ffmpeg: sudo ffmpeg -f v4l2 -i /dev/video1 -vframes 1 /tmp/frame.jpg ==="
