# HD60 Pro Driver - Historical Findings

This file is intentionally kept as a short historical pointer.

The older 2026-08-20 notes were useful while exploring, but several conclusions
are now obsolete:

- `/dev/video1` was a local enumeration artifact; current test scripts use
  `/dev/video0`.
- `bar5_dma_program` and `bar0_dma_advertise` were removed. Direct host-side
  BAR5/BAR0 writes looked promising but do not match the ARM firmware path.
- `cmd 0x06` is no longer sent by default. It is still available only through
  the `send_stream_start_cmd06=1` module parameter for controlled testing.
- The viable path is the firmware DMAC profile flow:
  `video_capture_mgr` / `libmassmemaccess.so.9` -> `/dev/vpl_dmac` ioctls ->
  `vpl_dmac.ko` -> `VPL_DMAC_StartTail()` -> `pcie_set_outbound()`.

Use `docs/claude-handoff-2026-08-22.md` for the current state and next steps.
