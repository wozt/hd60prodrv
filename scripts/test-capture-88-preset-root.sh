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
	allow_capture_88_writes=1 \
	request_irq_vector=1 \
	enable_v4l2=1 \
	prepare_dma_buffers=1 \
	mailbox_bar=0 >"$TMPDIR/load-safe.txt" 2>&1; then
	echo "== load-safe FAILED =="
	cat "$TMPDIR/load-safe.txt"
	exit 1
fi

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"

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

run_quiet preinit_command1 10 "$DBG/preinit_command1"
run_quiet fw_status_command10 10 "$DBG/fw_status_command10"
run_quiet gpio17_generic_sequence 15 "$DBG/gpio17_generic_sequence"
run_quiet i2c1a_c0_dc_db 10 "$DBG/i2c1a_c0_dc_db"
run_quiet logo_upload_all 40 "$DBG/logo_upload_all"
run_quiet pre_28548c_logo_uploads_local 40 "$DBG/pre_28548c_logo_uploads_local"
run_quiet post_logo_pipeline_28548c_min 10 "$DBG/post_logo_pipeline_28548c_min"
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
run_quiet post_logo_selector_shadow_probe 10 "$DBG/post_logo_selector_shadow_probe"
run_quiet post_logo_cmd1d_a2 10 "$DBG/post_logo_cmd1d_a2"
run_quiet post_logo_challenge_a2 10 "$DBG/post_logo_challenge_a2"

echo "== apply_capture_88_preset_2400_min =="
timeout 20s cat "$DBG/apply_capture_88_preset_2400_min"

echo
echo "== capture_info =="
cat "$DBG/capture_info"

echo
echo "== health =="
cat "$DBG/health"

echo
echo "== bar5_regs =="
cat "$DBG/bar5_regs"
