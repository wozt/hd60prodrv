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
- `stop_streaming()` now sends ARM-confirmed STOP_STREAMING `cmd 0x07` by
  default when real DMA capture was active. Disable with
  `send_stream_stop_cmd07=0` only for diagnostics.
- Added `scripts/load-vlc-source-root.sh` for the current user-facing load path.
- Added `scripts/load-vlc-real-root.sh` for the explicit real-DMA VLC load path
  (`synthetic_v4l2=0`, DMA buffers, IRQ, bus master, mailbox writes). It
  defaults to power-prep without PCI reset, `preinit_command1`, and base
  `firmware_load` before exposing the VLC path. Use `INIT_FIRST=0` only when
  the card is already initialized. It still falls back to black buffers when
  firmware produces no frame.
- Added `irq_mode=auto|intx|msi|msix` so IRQ selection can be forced during
  mailbox/stream diagnostics.
- Updated `preinit_command1` to match Windows/ARM more closely: 100 attempts by
  default, 2000 ms per attempt (`preinit_command1_attempts`,
  `preinit_command1_timeout_ms`). The ARM `MZ0380_HwInitialize` decompile passes
  `20000000` 100 ns units into `MZ0380_SendVendorCommand_P5` for command `0x01`,
  which becomes a 2 s wait in `MZ0380_WaitInterruptComplete`.
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
- Current local retest after the latest pushed diagnostics still fails before
  firmware load: `sudo ./scripts/test-after-cold-boot-real-frame-root.sh`
  reports `final_verdict: PREINIT_FAILED`, zero IRQ delta, BAR0+0x30 stays
  zero, and `final_doorbell_bar0_000: 0xffffffff`. This strongly indicates the
  endpoint/mailbox is still not accepting host doorbells in the current
  non-cold-boot state. A short 2-attempt validation after the classification
  patch now reports `classification: doorbell_all_ones_without_irq`.
- Added read-only `mailbox_compare` to dump the BAR0 and BAR5 candidate mailbox
  offsets side by side in the same hardware state. The cold-boot real-frame
  script now prints it before and after `preinit_command1`; use it to prove
  whether the failure is BAR0-specific or both candidate windows are inert.
  BAR5 was tested once with a single 20 ms preinit attempt and is not a safe
  mailbox alternative: it changed PCI config and both BAR windows to
  `0xffffffff`; `lspci` then reported `Unknown header type 7f`. The script now
  blocks `MAILBOX_BAR=5` unless `ALLOW_UNSAFE_MAILBOX_BAR5=1` is set.
- Added `scripts/pci-preflight-root.sh` and wired it into power prep and module
  load paths. If PCI config reads all `0xff` or header type is `0x7f/0xff`, the
  scripts now stop before `insmod` and ask for a full PSU cold boot.
- Aligned preinit timing with the ARM Linux driver: default
  `preinit_command1_timeout_ms` is now 2000 ms, and cold-boot wrapper timeouts
  default to 230 s so all 100 attempts can actually run.
- Inventoried the local DVP Linux SDK zip. `libqcap.x64.so` is a non-stripped
  userspace V4L2/QCAP library, not a kernel driver. It confirms mmap V4L2 as the
  correct Linux surface and shows `VIDIOC_S_PARM`; the driver now implements
  `VIDIOC_G_PARM/S_PARM` for fixed 1080p60 plus standard stored no-op V4L2
  controls for brightness/contrast/saturation/hue/audio volume/audio mute.
  Notes are in `docs/qcap-sdk-notes.md`, with a reproducible helper at
  `scripts/analyze-qcap-sdk.sh`.
- Rechecked the ARM Ghidra export in `/tmp/mz0380-ghidra-stream/`: its
  `MZ0380_StopFirmware` sends `[0x800, 0x07, 0xffffffff]` with the same
  `200000000` 100 ns timeout model as stream-start commands. Linux stream-off
  now mirrors that instead of tying `cmd 0x07` to experimental `cmd 0x06`.
- V4L2 input/audio probing was made less hostile to normal capture apps: HDMI
  no longer reports `NO_SIGNAL` by default, `report_input_no_signal=1` restores
  the old diagnostic behavior, and `VIDIOC_ENUMAUDIO/G_AUDIO/S_AUDIO` exposes a
  single HDMI stereo audio input with `V4L2_CAP_AUDIO`. ALSA/real decoded audio
  samples are still not implemented.
- `scripts/test-after-cold-boot-real-frame-root.sh` now logs
  `v4l2-ctl --all` and `--list-ctrls` before and after the stream attempt, so
  the next cold-boot run records input status/control/audio-probe compatibility
  together with the real-frame verdict.
- Fixed the guarded headerless-DMA retry path: `allow_dma_headerless_frames=1`
  now makes the DMA poll worker call the existing headerless delivery logic when
  payload bytes at `+0x1000` are nonzero but the 4-byte payload header is zero.
  Before this, the poll worker skipped those buffers before
  `hd60pro_deliver_dma_frame()` could accept them.
- V4L2 real-DMA stream start now clears all advertised DMA frame buffers and
  resets `last_frame_meta` before enabling capture. This prevents stale payload
  bytes from a previous attempt from becoming a false `HEADERLESS_DMA_CANDIDATE`
  or a stale frame in VLC after retrying.
- `stream_start_test` now performs the same DMA frame buffer/counter/metadata
  reset before sending `0x29 + 0x2a + 0x02`, so its no-V4L2 result is also
  fresh-run evidence instead of possibly reflecting bytes from an earlier
  attempt.
- V4L2 real-DMA stream-start hard errors now clean up correctly: queued VB2
  buffers are returned with `VB2_BUF_STATE_ERROR`, delayed works are cancelled,
  DMA capture and streaming state are disabled, and PCI bus mastering is
  cleared before returning the error to userspace. This avoids VLC/v4l2-ctl
  hanging on buffers owned by a failed start path.

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
  - `mailbox_compare`
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

New focused profile/request notes are in `docs/dmac-profile-notes.md`.

Useful new reverse facts from `/tmp/mma-set-options.txt`,
`/tmp/mma-start-one-frame.txt`, and `/tmp/mma-process-one-frame.txt`:

```text
MassMemAccess_SetOptions option 0x50:
  object+0x60 = (u8)opt+0x04
  object+0x61 = (u8)opt+0x08
  object+0x62 = (u8)opt+0x0c
  object+0x63 = 1

MassMemAccess_SetOptions option 0x17:
  validates base/size/stride/object+0x50 and rejects size&7
  for size > 0x800, splits work into (size + 0x7ff) >> 11 chunks

MassMemAccess_ProcessOneFrame mode 3:
  chunks are <=0x800 bytes
  object+0x58 = request+0x34 + (chunk << 11)
  object+0x5c = request+0x38 + (chunk << 11)
  object+0x54 = object+0x50 + chunk * (request+0x20 << 8)
  helper 0xa64 starts the DMAC profile and helper 0x9d4 waits
```

Current implementation implication: Linux `cmd 0x02` advertises the four
coherent host frame buffer addresses, but it is not yet proven to populate the
MassMemAccess request consumed by `StartOneFrame`/`ProcessOneFrame` or submit
`/dev/vpl_dmac` ioctl `0xde00`. The next useful reverse target is the
host-to-firmware event path that builds that request from Windows stream
commands, SET_VIC/channel_done, or tinyvenc/libvideocap state.

Do not add guessed DMAC MMR writes. Either trigger the firmware path that builds
the 0x3c profile, or mirror a fully decoded profile after request construction
is understood.

Additional tinyvenc link decoded in this pass:

```text
tinyvenc5/tinyvenc7/tinyvenc8 import:
  VideoCap_GetBufVIC
  VideoCap_StartVIC
  TK_MMA_StartOneFrame
  TK_MMA_ProcessOneFrame
  TK_MMA_WaitOneFrameComplete
  TK_MMA_SetOptions

TK_MMA_StartOneFrame/TK_MMA_ProcessOneFrame wrappers:
  if r0 == NULL: return -1
  massmem_object = *(u32 *)r0
  request = r0 + 4
  r1 -> wrapper+0x3c == request+0x38
  r2 -> wrapper+0x38 == request+0x34
  r3 -> wrapper+0x18 == request+0x14
  call MassMemAccess_StartOneFrame or MassMemAccess_ProcessOneFrame

VideoCap_GetBufVIC:
  ioctl(fd, 0x8078e303)
  stack+0x5c/+0x60/+0x64 -> MemMgr_GetVirtAddr -> output+0x38/+0x3c/+0x40
  stack+0x90 low 13 bits -> output+0x54
  stack+0x90 bits 16..28 -> output+0x50
```

This makes tinyvenc7's call sites for `VideoCap_GetBufVIC` and `TK_MMA_*` the
highest-value static reverse target. It should reveal how VIC buffer records
become MassMemAccess request fields and whether the host `cmd 0x02` addresses
are consumed in that bridge.

Decoded tinyvenc7 call-site pattern after that:

```text
Every visible TK_MMA_SetOptions call uses option 0x50:
  opt+0x00 = 0x50
  opt+0x04 = 0 or 1
  opt+0x08 = 0 or 1
  opt+0x0c = context value

Descriptor/control TK_MMA_StartOneFrame calls:
  r0 = video_state+0xb4 MMA wrapper
  r1 = 0x90000000
  r2 = MemBroker_GetPhysAddr(descriptor/control buffer)
  r3 = 0x10 or computed descriptor byte count

Payload/block TK_MMA_ProcessOneFrame calls:
  r0 = video_state+0xb8 MMA wrapper
  r1 = 0x90000000
  r2 = prepared broker/physical buffer pointer
  r3 = 0x1000 or h264_output_record+0x08 + 0x1000
```

So `0x90000000` is now the strongest decoded endpoint aperture constant in the
tinyvenc video DMA path. The unsolved host-side question is how the advertised
Linux `cmd 0x02` coherent buffers are bound to that firmware aperture.

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

New `cmd 0x02` guard:

The current decoded DMA-advertisement packet carries four 32-bit host buffer
addresses in dwords 5/7/9/11, with zeroes in the alternating dwords. Linux now
uses one shared builder for V4L2 real-DMA, `cmd02_dma_setup`, and
`stream_start_test`. If any coherent DMA buffer is above 4 GiB, the driver
refuses to send `cmd 0x02` and prints a `force_32bit_dma=1` hint. This avoids
silent address truncation and makes no-frame results meaningful.

New real-frame verifier:

```sh
sudo ./scripts/test-real-v4l2-frame-root.sh
```

This script runs `load-vlc-real-root.sh`, captures raw YUYV through `v4l2-ctl`,
and compares the sample against the exact synthetic fallback pattern
`16,128,16,128`. It exits 0 only for non-fallback frame data. A successful run
is still not final image-quality proof, but it is much stronger than "VLC
opened" because it distinguishes the fallback from actual hardware-produced
bytes.

New real-DMA polling fallback:

The V4L2 real-DMA path no longer depends only on a frame IRQ. While streaming
with `synthetic_v4l2=0`, the driver polls the four coherent DMA buffer headers
every `real_dma_poll_ms` milliseconds, default 16. If any header dword is
non-zero and <= the expected 1080p YUYV payload size, the driver copies
`dma_frame_cpu[buf] + 0x1000` into the queued vb2 buffer, clears the header, and
ACKs BAR0+0x50. The IRQ tasklet and poller now use the same delivery helper.

The driver deliberately ignores IRQ events with a zero or oversized DMA header.
This prevents a false "real frame" where an IRQ causes a stale zero buffer to
be copied to V4L2. `capture_info` now reports `real_dma_poll_ms` and
`dma_poll_count`.

Live test result after adding the poller:

```sh
PREINIT_TIMEOUT=25s FIRMWARE_LOAD_TIMEOUT=70s FRAMES=2 \
  sudo -E ./scripts/test-real-v4l2-frame-root.sh
```

Result was still fallback (`exit 2`). `preinit_command1` did not complete, so
`firmware_load` was skipped. V4L2 captured two fallback frames; after capture:
`pipeline_ready=0`, `irq_count=0`, `dma_frame_count=0`, `dma_poll_count=93`,
and BAR0+0x60..0x6c read back `0xffffffff`. This proves the new poller is
running and not falsely accepting empty DMA buffers, but the firmware did not
write host frame data in the current non-cold-boot state.

New preinit failure classification:

`preinit_command1` now reports aggregate attempt counters and a `classification`
field:

```text
preinit_completed
mailbox_or_mmio_dead
interrupt_without_completion
completion_changed_without_success
mailbox_silent_timeout
```

`scripts/test-after-cold-boot-root.sh` prints those fields in a condensed
`preinit summary`. A short non-cold-boot validation with
`preinit_command1_attempts=3 preinit_command1_timeout_ms=100` returned
`classification: mailbox_silent_timeout`, `timeout_count=3`,
`total_irq_delta=0`, `first_completion_change_attempt=0`.

Preinit now also mirrors the Windows post-loop ACK sequence after the retry
loop: `BAR5+0xdc=2`, mailbox `+0x30=0`, mailbox doorbell `+0x00=0x400`.
Short validation with two 50 ms attempts confirmed
`final_windows_ack_sequence: 1` and still classified the current state as
`mailbox_silent_timeout`.

Power-state observability:

`health` and `windows_preinit_state` now print `pci_current_state` from the
kernel PCI device. `test-after-cold-boot-root.sh` also prints sysfs
`power/control`, `power/runtime_status`, runtime active/suspended counters, and
`d3cold_allowed` before the preinit attempt. Read-only validation showed
`pci_current_state: 0` in the current boot state.

Base-firmware cold-boot script update:

`scripts/test-after-cold-boot-base-fw-root.sh` now prints the same PCI power
state and condensed `preinit summary` as the main cold-boot script. If preinit
succeeds, it also prints a condensed `firmware_load summary`.

The `firmware_load` debugfs node now emits a `classification` field:

```text
prepare_timeout
prepare_mailbox_or_mmio_dead
prepare_error
commit_timeout
commit_mailbox_or_mmio_dead
commit_error
firmware_load_completed
```

Use this script after PSU cold boot when the goal is to immediately test the
Windows base-firmware `0x0e/0x0f` path after a successful preinit.

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

After a full host power cycle, the most goal-directed test is:

```sh
sudo ./scripts/test-after-cold-boot-real-frame-root.sh
```

It keeps one module instance loaded and runs power prep, preinit, base firmware
load, V4L2 real-DMA streaming, and fallback-vs-real frame analysis. Its
`final_verdict` is the quickest answer to whether the current cold boot produced
hardware frame bytes.

Possible verdicts:

```text
PREINIT_TIMEOUT
PREINIT_FAILED
FIRMWARE_LOAD_TIMEOUT
FIRMWARE_LOAD_FAILED
V4L2_CAPTURE_TIMEOUT_OR_ERROR
FALLBACK_ONLY
HEADERLESS_DMA_CANDIDATE
REAL_DMA_METADATA_ONLY
REAL_DMA_FRAME_VERIFIED
REAL_FRAME_CANDIDATE
```

`REAL_DMA_FRAME_VERIFIED` requires both real-DMA metadata
(`last_frame_extra=0`, `dma_frame_count>0`) and non-fallback bytes in the saved
YUYV file. `REAL_DMA_METADATA_ONLY` means DMA metadata/counters moved, but the
captured file still looked fallback-like; treat it as evidence to inspect, not
as a working frame.

`V4L2_CAPTURE_TIMEOUT_OR_ERROR` means the `v4l2-ctl --stream-mmap` process
failed or exceeded `CAPTURE_TIMEOUT` (default `30s`). The verifier dumps
`capture_info` and `frame_buffer_peek` before unloading so the cold-boot window
still leaves useful evidence instead of hanging indefinitely.

If that script reaches firmware success but reports `FALLBACK_ONLY`, inspect
`capture_info` and then test exact Windows `0x2d` and `0x31` 1080p60 payloads
through `EXTRA_ARGS`. Start from `windows_stream_extra_commands` and
`decode-mz0380-stream-tables.py --module-args-template`, then map the sanitized
mode-table values feeding `MZ0380_StartFirmware.c` lines 1176..1311. Static
`scale_tb[4]` only proves the 1920x1080 row; the remaining fields must come
from runtime stream state or a Windows trace.

`frame_buffer_peek` now reports all four coherent DMA buffers, including header
dword 0 and whether header bytes or payload bytes at `+0x1000` are non-zero.
The cold-boot real-frame script prints this after streaming so a `FALLBACK_ONLY`
result can distinguish "no DMA wrote any buffer" from "DMA wrote a buffer but
V4L2 did not classify it as a frame".

`capture_info` now prints `last_frame_extra` with a legend:

```text
0 = real_dma
1 = synthetic_black
2 = dma_without_vb2_queue
3 = real_dma_timeout_black
```

This fixes an earlier ambiguity where the real-DMA timeout fallback could look
like a real DMA frame in metadata. Validation confirmed synthetic mode reports
`extra=1`, while a short real-DMA timeout run reports `extra=3`.

Guarded headerless-DMA experiment:

If `frame_buffer_peek` after streaming shows payload bytes at `+0x1000` are
non-zero but `header_payload=0`, the cold-boot script now reports
`HEADERLESS_DMA_CANDIDATE` and prints this retry hint:

```sh
EXTRA_ARGS='allow_dma_headerless_frames=1' \
  sudo -E ./scripts/test-after-cold-boot-real-frame-root.sh
```

That mode is disabled by default. It treats non-zero payload bytes with a zero
header dword as a full 1080p YUYV DMA frame, covering the possibility that the
firmware does not populate the 4-byte payload header Linux currently expects.
It now works through both IRQ and poll delivery paths; older builds only
accepted headerless buffers when an IRQ reached `hd60pro_deliver_dma_frame()`.
The DMA buffers are cleared on each V4L2 stream start, so any nonzero
`frame_buffer_peek` payload after the next cold-boot run should be fresh data
from that run.
`stream_start_test` also clears the same buffers before its mailbox-only start
sequence.

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
./scripts/decode-mz0380-stream-tables.py --module-args-template
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
The `--module-args-template` option prints the guarded module-parameter shape
for `stream_extra_primary_2d`, `stream_extra_secondary_2d`, and
`stream_extra_final_31`. It deliberately uses placeholders so guessed values do
not get mistaken for trace data.

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
