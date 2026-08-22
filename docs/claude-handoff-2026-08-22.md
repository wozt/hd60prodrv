# Claude Handoff - 2026-08-22

## Current Status

- The driver builds after the cleanup in this handoff.
- The module now registers a V4L2 node by default (`enable_v4l2=1` default).
- VLC/v4l2 can open the node without special parameters, but the current output
  is black fallback YUYV unless real firmware DMA starts producing frames.
- Real hardware frames are still not flowing to V4L2/VLC.
- Do not use PCI reset as the normal recovery path anymore. Public VFIO reports
  for `12ab:0380` and today's local tests both indicate bus/PM reset can leave
  the card enumerated but mailbox-deaf.
- Use `sudo ./scripts/prepare-device-power-root.sh` to pin power management
  (`power/control=on`, `d3cold_allowed=0`) without reset.
- If command `0x01` still gets zero IRQs after prior PCI resets, a full host
  power cycle is probably required before the firmware handshake can be
  trusted again.
- External reset-state references:
  `https://forums.unraid.net/topic/44969-help-passing-through-capture-card/`,
  `https://forum.level1techs.com/t/passing-through-elgato-capture-card/155610`,
  `https://www.reddit.com/r/VFIO/comments/kxhwef/passthrough_elgato_hd60_pro_capture_card_to/`.
- `/dev/video0` is the current expected V4L2 node in scripts.
- V4L2 real-DMA mode now has a timeout watchdog. When the firmware produces no
  frame, it logs a real-DMA timeout and delivers black fallback YUYV buffers so
  `v4l2-ctl`/VLC can keep moving while the DMAC path is still being decoded.
- V4L2 real-DMA mailbox startup uses `real_dma_cmd_timeout_ms` instead of fixed
  15 s command waits. Default is 3000 ms per command.
- `stop_streaming()` skips legacy `cmd 0x07` unless `send_stream_start_cmd06=1`
  was used. This avoids hanging userspace on stream close while `cmd 0x06` is
  disabled by default.
- Added `scripts/load-vlc-source-root.sh` for the current user-facing load path.
- Added `scripts/load-vlc-real-root.sh` for the explicit real-DMA VLC load path
  (`synthetic_v4l2=0`, DMA buffers, IRQ, bus master, mailbox writes). It
  defaults to power-prep without PCI reset, `preinit_command1`, and base
  `firmware_load` before exposing the VLC path. Use `INIT_FIRST=0` only when
  the card is already initialized. It still falls back to black buffers when
  firmware produces no frame.
- Added `irq_mode=auto|intx|msi|msix` so IRQ selection can be forced during
  mailbox/stream diagnostics.
- Updated `preinit_command1` to match Windows more closely: 100 attempts by
  default, 500 ms per attempt (`preinit_command1_attempts`,
  `preinit_command1_timeout_ms`).
- `hd60pro_mailbox_send_async_locked` now polls BAR0+0x30 for BIT(11) and runs
  the same ACK sequence as the IRQ handler if the status bit appears. This
  emulates the Windows ISR event path even if Linux interrupt delivery is wrong.
- Retest after that polling change still produced no BAR0+0x30 BIT(11), no
  IRQs, and `preinit_command1` timed out after 100 attempts. The current
  failure is not merely missed MSI/INTx delivery.
- Added read-only `windows_preinit_state`, which shows the BAR5 refs already
  match Windows (`0xfa000004`/`0xfa00005f`) but BAR0+0x30 stays zero and
  completion stays `0xcc800000`.
- Direct command `0x0a` also times out with `completion=0xcc800000`,
  `irq_delta=0`, and zero status/version fields. The current hardware state is
  silent for the whole early async mailbox path, not only command `0x01`.
- `fw_status_command10` now skips the extra marker wait after async timeout so
  dead-mailbox tests return promptly.
- Added `scripts/prepare-device-power-root.sh`.
- Added `scripts/test-after-cold-boot-root.sh`, the preferred first test after
  a full host power cycle. It does not call PCI reset; it snapshots
  `windows_preinit_state`, runs `preinit_command1`, and only sends `0x0a` if
  preinit succeeds.
- Updated older staged test scripts so `preinit_command1` gets 65 s instead of
  stale 10 s timeouts, and `fw_status_command10` gets 40 s.
- `scripts/load-initialized-root.sh` no longer says "synthetic mode" after
  running the post-logo real-DMA test path. It now points the reader at
  `stream_start_test` as the real frame/DMA verdict.
- `scripts/verify-capture-scaffold-root.sh` now loads the safe V4L2 fallback
  source directly instead of depending on the fragile full post-logo mailbox
  pipeline. Use it for userspace/V4L2 plumbing, not for real-frame proof.
- Safe V4L2 smoke retested after these script edits:
  `v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1` produced a
  4,147,200-byte YUYV frame. This validates the V4L2 node/fallback path only.

## What Changed In This Pass

- Removed obsolete debugfs diagnostics:
  - `bar5_dma_program`
  - `bar0_dma_advertise`
- Removed the now-unreachable C backing code and module parameters for the old
  logo upload, post-logo challenge, and chip `0x88` preset debugfs endpoints.
  `verify-capture-scaffold-root.sh` now reads `windows_stream_extra_commands`
  and `windows_stream_scale_table` instead of the removed `0x88` nodes.
- Removed the old logo/GPIO/I2C/post-logo replay scripts from the active tree.
  They were useful historical probes, but they steer away from the current
  Windows-confirmed preinit/firmware mailbox path.
- `firmware_load` now supports `firmware_load_mode=base|full`:
  - `base` sends Windows `0x0e` prepare with `firmware_base_selector`, copies
    firmware to BAR0+0x60, then sends `0x0f` commit with a 180 s timeout.
  - `full` sends Windows `0x0b`, copies to BAR0+0x60, then sends `0x0c` commit
    with a 60 s timeout. It still requires `allow_unsafe_visible_fw_prepare=1`
    because cold `0x0b` previously made the card read as `0xffffffff`.
- Replaced the old `FINDINGS.md` with a historical pointer because it still
  claimed `/dev/video1`, direct BAR5 DMA programming, and default `cmd 0x06`.
- Kept useful debugfs tools:
  - `cmd02_dma_setup`
  - `setvic_inject`
  - `stream_start_test`
  - `frame_buffer_peek`
  - `firmware_pcie_outbound_regs`
  - `firmware_dmac_outbound_path`
  - `firmware_audio_path`

## Important Conclusion

Do not continue the direct host-side BAR5/BAR0 write path.

The strongest current evidence says the real outbound/DMA setup is firmware
owned:

```text
video_capture_mgr / libmassmemaccess.so.9
  -> /dev/vpl_dmac ioctls
  -> vpl_dmac.ko
  -> VPL_DMAC_StartTail()
  -> pcie_set_outbound()
```

`ep.ko` shows `pcie_set_outbound()` writing the endpoint outbound registers:

```text
selected_channel+0x50 = 1
selected_channel+0x54 = caller arg0
selected_channel+0x58 = caller arg1
selected_channel+0x74 = 0x90000000
selected_channel+0x7c = 0x91ffffff
selected_channel+0xd4 = 0x00f00000
```

`vpl_dmac.ko` shows `VPL_DMAC_StartTail()` calls:

```text
pcie_set_outbound(profile+0x38, profile+0x39, profile+0x3a)
```

but only after firmware userspace built and submitted a valid 0x3c-byte DMAC
profile.

## Reverse Engineering Notes

Useful ARM/Linux firmware root:

```text
/tmp/claude-1000/-home-wozt-hd60prodrv/d9769f12-0974-4a61-8572-41b3cab7d6cb/scratchpad/fw_extract/yuan_demo_sdi
```

Known useful files:

```text
drivers/ep.ko
drivers/vpl_dmac.ko
drivers/vpl_vic.ko
video_capture_mgr
libmassmemaccess.so.9
libvideocap.so.13
```

Use the local disassembler helper:

```sh
.venv-re/bin/python scripts/arm-ko-disasm.py /tmp/claude-1000/-home-wozt-hd60prodrv/d9769f12-0974-4a61-8572-41b3cab7d6cb/scratchpad/fw_extract/yuan_demo_sdi/drivers/vpl_dmac.ko VPL_DMAC_StartTail VPL_DMAC_ISRTail Ioctl
.venv-re/bin/python scripts/arm-ko-disasm.py /tmp/claude-1000/-home-wozt-hd60prodrv/d9769f12-0974-4a61-8572-41b3cab7d6cb/scratchpad/fw_extract/yuan_demo_sdi/libmassmemaccess.so.9 MassMemAccess_OpenDMAC MassMemAccess_StartDMAC MassMemAccess_WaitDMAC MassMemAccess_CloseDMAC
```

Already decoded from `libmassmemaccess.so.9`:

```text
MassMemAccess_StartDMAC:
  profile backing buffer: object+0x4c
  profile length copied/flushed: 0x3c
  uses MemMgr_GetPhysAddr for buffers
  calls MemMgr_CacheCopyBack(..., 0x3c)
  loops on ioctl(fd, 0xde00)

MassMemAccess_WaitDMAC:
  loops on ioctl(fd, 0xde01)

vpl_dmac.ko Ioctl:
  0xde00 -> VPL_DMAC_StartHead, then VPL_DMAC_StartTail
  0xde01 -> wait for channel completion
  0x8004de03 -> version/check path
  0x4004de02 -> MMR mapping/setup path
```

New detail decoded after the first handoff:

```text
MassMemAccess_StartDMAC profile build:
  object+0x4c -> profile pointer
  profile+0x08 = control word with:
    bits from object+0x20 << 4
    bits from object+0x24 << 6
    bits from object+0x14 << 10
    mode bits from object+0x1c: 0 => none, 1 => +0x100, 2 => +0x200,
      other => +0x300
  profile+0x10 = phys(object+0x58) or raw object+0x58 depending object+0x48
  profile+0x14 = object+0x5c or phys(object+0x5c)
  profile+0x18 = object+0x28
  profile+0x1c = object+0x30
  profile+0x20 = object+0x34
  profile+0x24 = object+0x38
  profile+0x28 = object+0x3c
  profile+0x2c = object+0x40
  profile+0x30 = phys(object+0x54), only on some modes
  profile+0x34 = object+0x44
  profile+0x38 = object+0x60, then VPL_DMAC_StartTail reads it as byte
  profile+0x39..0x3b are adjacent bytes from object+0x60..0x63

VPL_DMAC_StartTail:
  stores profile pointer into a 64-slot ring
  copies profile+0x08 and +0x10..+0x34 directly to DMAC MMR +0x08 and +0x10..+0x34
  if profile byte +0x3b != 0, calls pcie_set_outbound(profile[0x38],
    profile[0x39], profile[0x3a])
  finally writes profile+0x08 | 6 to MMR +0x08

Implication:
  pcie_set_outbound arguments are byte-sized control/channel values, not the
  host frame address directly. The frame addresses are in the copied MMR fields.
```

Firmware VIC/tinyvenc path decoded:

```text
video_capture_mgr SET_VIC event:
  chooses YUY2/YV12 sensor_config
  launches tinyvenc7 or tinyvenc5 with geometry/color args

libvideocap.so.13:
  VideoCap_Start -> VideoCap_StartVIC
  VideoCap_StartVIC sets VIC control bits and loops ioctl(/dev/vpl_vic, 0xe313)
  VideoCap_GetBufVIC uses ioctl(/dev/vpl_vic, 0x8078e303) for a 0x78-byte
    buffer/frame record

vpl_vic.ko:
  ioctl 0xe313 is the VIC start/wait transition used by tinyvenc
  ioctl 0x8078e303 copies the 0x78-byte GetBufVIC record to userspace
```

So the host-side missing piece is still before VIC/DMAC: reliable endpoint
event/hready transport that makes `video_capture_mgr` launch tinyvenc after
SET_VIC.

Additional Windows stream-start caveat:

`/home/wozt/mz0380-decompiled/MZ0380_StartFirmware.c` shows that Windows can
continue after `cmd 0x29` and `cmd 0x2a` with format/scaler commands `0x2d`
and `0x31` when the earlier command-success bits are set. Linux currently
sends only `0x29 + 0x2a + 0x02` in `stream_start_test`/V4L2 real-DMA mode.
Do not blindly add `0x2d/0x31` to the normal path: their payloads are assembled
from several mode-table variables. Decode the exact 1080p60 values first or
add them behind an explicit experimental module parameter.

## Audio Reverse Engineering Update

Audio is still not implemented as a Linux ALSA capture device. New static notes
are in `docs/audio-driver-notes.md`, and the driver exposes the same summary at:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/firmware_audio_path
```

Key decoded firmware facts:

```text
audio_capture_mgr:
  opens /sys/vpl_pciep/audio_ctrl
  command 0x2a = Set AIC params
  fields printed by firmware:
    bits, channel_num, mono, freq, frame_num_of_period,
    period_num_of_buffer, on
  HD launch:
    ./capture_audio_8ch -D -P <periods> -d <i2s> -R <freq> -F <frames> -B <bits>
  toggles /sys/vpl_pciep/hready

capture_audio_8ch:
  main candidate 0x8eb8, PCM/MMA loop 0x9948
  opens hw:0,0..hw:0,3
  configures stereo S16_LE PCM per device
  HD hint string recommends 48000 Hz, F=256, P=4
  reads PCM with snd_pcm_readi()
  uses MemBroker_GetMemory / MemBroker_GetPhysAddr
  calls TK_MMA_ProcessOneFrame()
  writes 0x18 bytes to /sys/class/vpl_pciep/channel_done
```

Implementation implication: do not add fake/silent ALSA as the audio proof. Map
the real firmware `channel_done` record and host raw-audio tasklet/ring, then
expose that as ALSA capture.

Linux now has a read-only debugfs node for this decode:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_extra_commands
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_scale_table
```

It documents the success gates and packet shapes for the primary/secondary
`0x2d` commands and the final `0x31` command. It does not send hardware
commands. Validation done: `make` passes, read-only load with
`enable_v4l2=0 allow_mailbox_writes=0` exposed the node, and the module was
unloaded afterward.

New guarded test hook:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/stream_extra_command_send
```

It sends nothing by default. It only sends raw packets supplied through module
parameters, and only when `allow_stream_extra_commands=1` is set:

```text
stream_extra_primary_2d=0x800,0x2d,<10 more dwords>
stream_extra_secondary_2d=0x800,0x2d,<10 more dwords>
stream_extra_final_31=0x800,0x31,<5 more dwords>
```

If `send_stream_extra_commands=1` is also set, the V4L2 real-DMA startup and
`stream_start_test` insert those packets after `0x29/0x2a` and before `0x02`.
This is deliberately for exact Windows trace payloads only; do not use guessed
values.

Added offline extractor:

```sh
./scripts/decode-mz0380-stream-tables.py
```

Useful fixed table facts from `LXV4L2D_MZ0380.ko`:

```text
.data+0x1ed8 scale_tb, size 240
scale_tb[4] = 1920 x 1080 (0x0780 x 0x0438)
.data+0x15798 SC2CC_VIN_MAP = 0,2,1,3,4,6,5,7
.data+0x1fc8 TABLE_DEVICE_INPUT_TOPOLOGY, size 4208
```

Negative result: fixed tables are not enough to emit safe `0x2d/0x31` packets.
The AArch64 disassembly shows those packets also consume runtime stream-state
fields copied into stack temporaries around `sp+0x140..0x240`.

## Suggested Next Target

After a full host power cycle, first run:

```sh
sudo ./scripts/test-after-cold-boot-root.sh
```

If `preinit_command1` succeeds again, run the post-logo path and inspect
`stream_start_test`. If `0x29/0x2a/0x02` still produces no DMA frames, the next
static target is the exact Windows `0x2d` and `0x31` 1080p60 payload sequence.
Start from `windows_stream_extra_commands`, then map the sanitized mode-table
values feeding `MZ0380_StartFirmware.c` lines 1176..1311. Static `scale_tb[4]`
only proves the 1920x1080 row; the remaining fields must come from runtime
stream state or a Windows trace.

Also map the 0x3c-byte `MassMemAccess_StartDMAC` profile completely:

- Which bytes/words at `profile+0x08..0x34` become DMAC MMR registers.
- Which object fields produce `profile+0x38`, `profile+0x39`, `profile+0x3a`,
  and `profile+0x3b`.
- Which caller in `video_capture_mgr` or `libvideocap.so.13` configures the
  capture frame transfer.

Then implement a guarded Linux diagnostic that either triggers the proper
firmware event/config path or mirrors the exact firmware DMAC profile. Avoid
another speculative BAR register poke unless the firmware layout proves it.

## 2026-08-22 Late Update: 0x2d/0x31 Trace Targets

`MZ0380_StartFirmware` sends optional post-SET_VIC commands only after success
bits are set:

- `0x29` success sets `stream_state[0x639] |= 0x100`.
- `0x2a` success sets `stream_state[0x639] |= 0x200`.
- Primary `0x2d` is gated by `(success_bits & 0x300) == 0x300` and sets
  `0x400`.
- Secondary `0x2d` is gated by `(success_bits & 0x700) == 0x700` and sets
  `0x800`.
- Final `0x31` is gated by `(success_bits & 0xf00) == 0xf00` and sets
  `0x1000`.

Do not hard-code these packets yet. The fixed `scale_tb` row for 1920x1080 is
known, but `0x2d/0x31` mostly use runtime fields copied from the ARM driver
context. Trace or reconstruct these exact word offsets:

```text
param_1+0x1818 stream_state:
  +0x013 +0x020 +0x025 +0x02a +0x02f +0x034 +0x039 +0x044 +0x62d +0x639

param_1+0x1944 window_table_a:
  +0x000 +0x008 +0x020 +0x028 +0x040 +0x048 +0x060 +0x068 +0x080 +0x088

param_1+0x14878 window_table_b:
  +0x000 +0x008 +0x040 +0x160 +0x1a0 +0x1a8 +0x1c0 +0x1c8
  +0x1e0 +0x1e8 +0x200 +0x208 +0x220 +0x228 +0x240 +0x248
  +0x260 +0x268 +0x280 +0x288 +0x2a0 +0x2a8 +0x2c0 +0x2c8
  +0x320 +0x328 +0x330 +0x338

param_1+0x14000 board_state:
  +0x08b +0x08c +0x08d +0x08e +0x08f +0x597 +0x598 +0x5b1
  +0x5c4 +0x5c5 +0x5cf
```

The helper now prints this list:

```sh
./scripts/decode-mz0380-stream-tables.py
./scripts/decode-mz0380-stream-tables.py --packet-model
```

The debugfs node `windows_stream_extra_commands` also prints the trace targets.

Fresh Ghidra headless export was validated with:

```sh
/home/wozt/logiciels/ghidra_12.1.2_PUBLIC/support/analyzeHeadless \
  /home/wozt/ghidra-projects MZ0380 \
  -process LXV4L2D_MZ0380.ko -noanalysis \
  -scriptPath scripts/ghidra \
  -postScript ExportMZ0380Stream.java /tmp/mz0380-ghidra-stream
```

The fresh export corrected a packet-order risk: the `0x2d` reduced scaler word
is packet `w7`; `w6` is the row/timing/misc word. Trust
`decode-mz0380-stream-tables.py --packet-model` over older hand notes.

## Test Commands

Build:

```sh
make
```

Prepare the device without reset:

```sh
sudo ./scripts/prepare-device-power-root.sh
```

First test after a full power cycle:

```sh
sudo ./scripts/test-after-cold-boot-root.sh
```

If `preinit_command1` succeeds after the cold power cycle, the next guarded
firmware test is:

```sh
sudo rmmod hd60prodrv 2>/dev/null || true
sudo insmod ./hd60prodrv.ko enable_v4l2=0 mailbox_bar=0 request_irq_vector=1 allow_mailbox_writes=1 allow_preinit_command1=1 allow_firmware_load=1 firmware_load_mode=base firmware_name=hd60prodrv/MZ0380.HD.HEX
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/preinit_command1
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/firmware_load
sudo rmmod hd60prodrv
```

Last-resort reset only:

```sh
sudo ./scripts/recover-device-root.sh
```

V4L2 synthetic sanity:

```sh
sudo ./scripts/load-safe.sh ./hd60prodrv.ko
timeout 15s sudo v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=5 --stream-to=/tmp/hd60pro-synth.yuyv
sudo rmmod hd60prodrv
```

User-facing VLC source:

```sh
sudo ./scripts/load-vlc-source-root.sh
vlc v4l2:///dev/video0
```

V4L2 real-DMA no-frame behavior should now timeout instead of hanging forever:

```sh
sudo ./scripts/load-safe.sh ./hd60prodrv.ko mmio_dump=1 enable_busmaster=1 request_irq_vector=1 enable_v4l2=1 prepare_dma_buffers=1 force_32bit_dma=1 synthetic_v4l2=0 allow_dma_capture=1 allow_mailbox_writes=1 mailbox_bar=0
timeout 15s sudo v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=5 --stream-to=/tmp/hd60pro-real.yuyv
sudo rmmod hd60prodrv
```

## Pitfalls

- If mailbox/debugfs reads return `0xffffffff` or time out, stop testing and
  avoid repeated PCI reset. Use `prepare-device-power-root.sh`; if the mailbox
  remains deaf, do a full power cycle.
- `preinit_command1` now loops up to 100 times by default. On 2026-08-22 it
  still failed after repeated PCI reset with:
  `attempts_run=100 result=-110 irq_delta=0 completion=0xcc800000`.
- Retest on the same still-running boot after the late 0x2d/0x31 notes also
  failed the no-reset `test-after-cold-boot-root.sh` path:
  `attempts_run=100 result=-110 completion=0xcc800000 irq_delta=0`,
  `bar0_000=0xffffffff`, `bar0_030=0`, `bar5_refs_match_windows_preinit=1`.
  This confirms the current boot remains mailbox-deaf; use a full PSU power-off
  cold boot before spending more time on real-frame tests.
- `cmd 0x06` may cancel or confuse capture. It is skipped by default and should
  only be tested with `send_stream_start_cmd06=1`.
- The older `FINDINGS.md` content is intentionally gone because it was steering
  the work toward obsolete assumptions.
