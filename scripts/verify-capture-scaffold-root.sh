#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

./scripts/load-initialized-root.sh >/tmp/hd60prodrv-load-initialized.log

DBG="/sys/kernel/debug/hd60prodrv/0000:22:00.0"

echo "== capture_start_plan =="
cat "$DBG/capture_start_plan"

echo
echo "== dma_info =="
cat "$DBG/dma_info"

echo
echo "== firmware_endpoint_tables =="
cat "$DBG/firmware_endpoint_tables"

echo
echo "== endpoint_command_plan =="
cat "$DBG/endpoint_command_plan"

echo
echo "== set_vic_event_record =="
cat "$DBG/set_vic_event_record"

echo
echo "== endpoint_transport_plan =="
cat "$DBG/endpoint_transport_plan"

echo
echo "== windows_payload_uploader =="
cat "$DBG/windows_payload_uploader"

echo
echo "== windows_stream_state_flow =="
cat "$DBG/windows_stream_state_flow"

echo
echo "== windows_stream_consumers =="
cat "$DBG/windows_stream_consumers"

echo
echo "== windows_frame_counter_info =="
cat "$DBG/windows_frame_counter_info"

echo
echo "== windows_directmemory_drain_path =="
cat "$DBG/windows_directmemory_drain_path"

echo
echo "== windows_contiguous_buffer_layout =="
cat "$DBG/windows_contiguous_buffer_layout"

echo
echo "== windows_buffer_queue_info =="
cat "$DBG/windows_buffer_queue_info"

echo
echo "== windows_frame_producer_search =="
cat "$DBG/windows_frame_producer_search"

echo
echo "== windows_external_buffer_info =="
cat "$DBG/windows_external_buffer_info"

echo
echo "== windows_dma_mapping_info =="
cat "$DBG/windows_dma_mapping_info"

echo
echo "== windows_dma_publish_search =="
cat "$DBG/windows_dma_publish_search"

echo
echo "== windows_bar_mapping_xrefs =="
cat "$DBG/windows_bar_mapping_xrefs"

echo
echo "== windows_worker_event_path =="
cat "$DBG/windows_worker_event_path"

echo
echo "== windows_event_queue_xrefs =="
cat "$DBG/windows_event_queue_xrefs"

echo
echo "== windows_event_callback_bridge =="
cat "$DBG/windows_event_callback_bridge"

echo
echo "== windows_stream_callback_search =="
cat "$DBG/windows_stream_callback_search"

echo
echo "== windows_physical_buffer_xrefs =="
cat "$DBG/windows_physical_buffer_xrefs"

echo
echo "== firmware_userland_flow =="
cat "$DBG/firmware_userland_flow"

echo
echo "== endpoint_bridge_regs =="
cat "$DBG/endpoint_bridge_regs"

echo
echo "== firmware_pcie_outbound_regs =="
cat "$DBG/firmware_pcie_outbound_regs"

echo
echo "== firmware_dmac_outbound_path =="
cat "$DBG/firmware_dmac_outbound_path"

echo
echo "== windows_88_update_plan =="
cat "$DBG/windows_88_update_plan"

echo
echo "== windows_calibration_info =="
cat "$DBG/windows_calibration_info"

echo
echo "== windows_bridge_attach_info =="
cat "$DBG/windows_bridge_attach_info"

echo
echo "== windows_capture_88_presets =="
cat "$DBG/windows_capture_88_presets"

echo
echo "== windows_88_mask_tables =="
cat "$DBG/windows_88_mask_tables"

if command -v v4l2-ctl >/dev/null 2>&1; then
	echo
	echo "== v4l2_stream_smoke =="
	v4l2-ctl -d /dev/video0 --stream-mmap=2 --stream-count=2 \
		--stream-to=/tmp/hd60prodrv-smoke.yuyv

	echo
	echo "== direct_memory_info_after_stream =="
	cat "$DBG/direct_memory_info"
fi
