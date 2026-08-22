# hd60prodrv

Experimental Linux bring-up driver for the Elgato Game Capture HD60 Pro PCIe
card seen as:

```text
12ab:0380 subsystem 1cfa:0006
YUAN High-Tech Development Co., Ltd. / Corsair-Elgato
```

Current status: the module registers a V4L2 capture node by default and VLC can
open it. Until the HD60 Pro firmware DMA path is fully decoded, the driver emits
black YUYV fallback frames instead of real HDMI frames when hardware DMA produces
no frame. Low-level PCI, mailbox, and firmware diagnostics remain available
through debugfs.

## Current Local Findings

On this machine:

```text
BDF:              0000:22:00.0
PCI ID:           12ab:0380
Subsystem:        1cfa:0006
Class:            0x000000
BAR0:             32 MiB MMIO at 0xfa000000
BAR5:             4 KiB MMIO at 0xfc000000
IOMMU group:      13
Kernel:           6.12.94+deb13-amd64
```

The running kernel currently has no build tree at
`/lib/modules/$(uname -r)/build`, so the module cannot be compiled until the
matching Debian kernel headers are installed.

## Public Research Summary

Useful sources:

- <https://github.com/tolga9009/elgato-gchd> is for the older USB Elgato Game
  Capture HD. Its README explicitly lists HD60 Pro as unsupported, so it is useful
  only as reverse-engineering background, not as a direct driver base.
- <https://github.com/stoth68000/sc0710> and
  <https://github.com/Nakildias/sc0710> are PCIe/V4L2 drivers for Elgato 4K60
  Pro MK.2 / 4K Pro hardware using `12ab:0710`. They are the closest Linux PCIe
  architecture reference, but they do not directly support this `12ab:0380`
  HD60 Pro card.
- Elgato's public specs say HD60 Pro is PCIe 2.0 x1, HDMI input/output, up to
  1080p60, and Windows-only officially.
- Public PCI ID databases list HD60 Pro variants for `12ab:0380`, including
  subsystem IDs `1cfa:0003`, `1cfa:0005`, and `1cfa:0006`.

## Build

Install matching kernel headers first. On Debian this is usually:

```sh
sudo apt install "linux-headers-$(uname -r)" build-essential
```

Then:

```sh
make
```

Prepare the official firmware files for future `request_firmware()` experiments:

```sh
./scripts/extract-elgato-driver.sh
sudo ./scripts/install-firmware-root.sh
```

## Secure Boot

If `insmod` fails with `Key was rejected by service`, Secure Boot/module
signature enforcement is active. Either disable Secure Boot in firmware setup, or
sign the module with a locally enrolled MOK:

```sh
./scripts/secureboot-generate-key.sh
sudo mokutil --import certs/MOK.der
sudo reboot
```

During reboot, MokManager will ask to enroll the key. After enrollment:

```sh
cd /home/wozt/hd60prodrv
make
./scripts/secureboot-sign-module.sh ./hd60prodrv.ko
sudo ./scripts/run-snapshot-root.sh
```

If an old snapshot is stuck on a wide BAR0 debugfs read:

```sh
sudo ./scripts/recover-stuck-snapshot-root.sh
```

## Load And Open In VLC

Default load registers `/dev/video*`:

```sh
sudo ./scripts/load-safe.sh ./hd60prodrv.ko
v4l2-ctl --list-devices
vlc v4l2:///dev/video0
```

If another camera already owns `/dev/video0`, use the node shown by
`v4l2-ctl --list-devices`.

The current fallback stream is intentionally black. Real HDMI capture still
depends on completing the firmware DMAC reverse engineering described in
`docs/claude-handoff-2026-08-22.md`.

The explicit real-DMA VLC load path is:

```sh
sudo ./scripts/load-vlc-real-root.sh
vlc v4l2:///dev/video0
```

By default this prepares power without PCI reset, runs `preinit_command1`, then
runs base firmware load before exposing the V4L2 real-DMA path. Set
`INIT_FIRST=0` only when the card is already initialized. This still falls back
to black buffers when the firmware produces no frame, but it exercises the
mailbox/DMA startup path instead of pure synthetic mode.

The decoded command `0x02` DMA advertisement currently carries four 32-bit host
buffer addresses. The real-DMA scripts therefore load with `force_32bit_dma=1`.
If a future manual load omits that and coherent buffers land above 4 GiB, the
driver refuses to send `cmd 0x02` instead of silently truncating DMA addresses.

Real-DMA mode also polls coherent DMA frame headers by default
(`real_dma_poll_ms=16`) so a hardware frame can still reach V4L2 if the firmware
writes host memory before the frame IRQ path is fully decoded. Frames are only
delivered as real data when the DMA header payload dword is non-zero and fits
the 1080p YUYV frame size; otherwise the normal timeout fallback remains black.

To test whether V4L2 is delivering real hardware data instead of the synthetic
black fallback:

```sh
sudo ./scripts/test-real-v4l2-frame-root.sh
```

The script loads the real-DMA path, captures raw YUYV with `v4l2-ctl`, and
returns success only when captured frame data differs from the exact fallback
pattern and is not just an empty zero buffer.

Audio capture is not exposed as ALSA yet. The decoded firmware audio path is
tracked in `docs/audio-driver-notes.md`.

## Load For Diagnostics

Initial safe load:

```sh
sudo ./scripts/load-safe.sh
dmesg | grep hd60prodrv
```

Optional MMIO header dump through debugfs:

```sh
sudo modprobe debugfs
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
sudo rmmod hd60prodrv
sudo insmod ./hd60prodrv.ko mmio_dump=1
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/info
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/firmware_info
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/pci_config
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/bar5_head
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/bar5_regs
```

`enable_busmaster=1` still stays off until DMA is understood.
`request_irq_vector=1` is now available for a controlled interrupt experiment:
it requests one vector and uses the BAR5 interrupt acknowledge sequence found in
the Windows driver. It does not start capture or DMA.

```sh
sudo rmmod hd60prodrv 2>/dev/null || true
sudo ./scripts/load-safe.sh ./hd60prodrv.ko mmio_dump=1 request_irq_vector=1
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/info
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/bar5_regs
```

If the IRQ experiment loads but `irq_count` stays at zero, trigger a visible HDMI
change, for example unplug/replug the HDMI input or change the source resolution,
then read `info` again.

Experimental mailbox firmware-version query:

```sh
sudo rmmod hd60prodrv 2>/dev/null || true
sudo ./scripts/load-safe.sh ./hd60prodrv.ko mmio_dump=1 allow_mailbox_writes=1
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/fw_version
sudo cat /sys/kernel/debug/hd60prodrv/0000:22:00.0/bar5_regs
sudo dmesg | grep hd60prodrv | tail -50
```

This sends the Windows-observed mailbox packet `0x800, 0x1c, 0xa3` and reports
the completion register plus the response dwords. Keep `enable_busmaster=0`.
If BAR5 starts reading `0xffffffff` afterward, the device did not accept this
command in its current state. First pin the device out of runtime power saving
without resetting it:

```sh
sudo ./scripts/prepare-device-power-root.sh
```

Avoid PCI reset as the normal recovery path for this card. Public VFIO reports
for `12ab:0380` and the local pre-init tests both point to bus/PM reset causing
a device that enumerates but no longer answers the firmware mailbox. The old
reset helper is still present for last-resort recovery:

```sh
sudo ./scripts/recover-device-root.sh
```

If the kernel reports `Unable to change power state from D3cold to D0` or PCI
config reads as `0xffff`, or if command `0x01` gets zero IRQs after a PCI reset,
a full power cycle is required. Shut the machine down, switch off/disconnect PSU
power for a few seconds, then boot again.

After a full power cycle, do not run the PCI reset helper first. Run the focused
mailbox bring-up probe:

```sh
sudo ./scripts/test-after-cold-boot-root.sh
```

This script pins power management without reset, records `windows_preinit_state`,
runs the Windows-style `preinit_command1`, and sends `fw_status_command10` only
if preinit completes.

To use the cold-boot window for the full frame objective, run the one-shot
real-frame path instead:

```sh
sudo ./scripts/test-after-cold-boot-real-frame-root.sh
```

It keeps one module instance loaded, runs preinit, base firmware load, V4L2
real-DMA streaming, and raw frame analysis. Success means non-fallback YUYV data
was captured from `/dev/video0`.

## Development Plan

1. Bring-up and inventory
   - Bind safely to `12ab:0380`.
   - Record BAR sizes, PCI config, subsystem variants, power state, IRQ/MSI
     capabilities, and stable read-only register regions.
   - Compare cold boot, warm boot after Windows driver, and after HDMI source
     changes.

2. Register reverse engineering
   - Capture Windows PCI config and MMIO traces if possible.
   - Identify blocks for control/status, HDMI receiver, EDID, scaler/encoder,
     DMA rings, and interrupt status/ack.
   - Avoid blind writes; introduce named registers only when backed by traces or
     repeatable observations.

3. Minimal V4L2 node
   - Register a `/dev/video*` node once signal detection and frame dimensions are
     understood.
   - Start with a single conservative format such as YUYV 1920x1080@60 if the
     hardware DMA path confirms that layout.

4. DMA capture
   - Implement videobuf2 queues.
   - Allocate coherent descriptors and streaming buffers.
   - Enable bus mastering only around verified DMA programming.
   - Add streaming interrupt handling once mailbox commands and DMA descriptors
     are understood. The current IRQ path is diagnostic only.

5. HDMI/EDID/audio
   - Implement EDID get/set if supported by the card.
   - Add ALSA capture after audio packet/ring layout is identified.
   - Handle no-signal, resolution changes, and HDCP rejection cleanly.

6. Packaging and safety
   - DKMS packaging.
   - `modprobe.d` defaults with dangerous diagnostics disabled.
   - Reproducible diagnostic script for bug reports.

## Test Commands

```sh
./scripts/diag.sh
make
sudo ./scripts/load-safe.sh
sudo rmmod hd60prodrv
```

One-shot loaded snapshot:

```sh
sudo ./scripts/run-snapshot-root.sh
```

Compare two BAR5 register snapshots:

```sh
./scripts/diff-bar5-regs.sh snapshots/<a>-loaded snapshots/<b>-loaded
```
