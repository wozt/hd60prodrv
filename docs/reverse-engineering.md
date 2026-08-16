# Reverse Engineering Workflow

The HD60 Pro `12ab:0380` should be treated as a different device from the
`12ab:0710` cards supported by `sc0710`. Reusing the V4L2/videobuf2 shape from
`sc0710` is reasonable; reusing register writes is not safe until the register
map is proven.

## Baseline Snapshots

Collect snapshots in these states:

1. Linux cold boot, no HDMI source.
2. Linux cold boot, HDMI source connected and outputting 1080p60.
3. Linux after Windows has initialized the card, then warm reboot into Linux.
4. Linux after changing source resolution.
5. Linux after unplug/replug HDMI.

For each state:

```sh
make
sudo ./scripts/load-safe.sh build/hd60prodrv.ko
sudo ./scripts/snapshot.sh 0000:22:00.0 snapshots/<state-name>
sudo ./scripts/unload.sh
```

For MMIO header reads:

```sh
sudo insmod build/hd60prodrv.ko mmio_dump=1
sudo ./scripts/snapshot.sh 0000:22:00.0 snapshots/<state-name>-mmio
sudo rmmod hd60prodrv
```

## What To Identify

- Stable identity registers: magic values, firmware/FPGA version, board revision.
- Volatile status registers: HDMI plugged, signal lock, resolution, frame rate,
  colorspace, HDCP rejection.
- Control registers: reset blocks, EDID SRAM/address window, scaler/encoder
  mode, DMA engine enable.
- DMA layout: descriptor format, ring base registers, stride, pitch, pixel
  format, frame completion markers.
- Interrupts: vector type, status bits, ack register, error bits.

## Windows Trace Targets

The highest-value trace is the Windows driver programming sequence:

- PCI config before and after driver load.
- BAR0/BAR5 writes during device start.
- BAR writes when HDMI source locks.
- BAR writes when capture starts/stops in OBS or Elgato software.
- DMA buffer physical addresses/descriptors if the tracing tool exposes them.

Useful approaches:

- Windows VM with PCI passthrough and hypervisor MMIO logging, if your platform
  can isolate the capture card.
- Windows kernel debugging or driver instrumentation on bare metal.
- Compare Linux cold boot snapshots against warm reboot snapshots after Windows
  has initialized the card.

## Driver Milestones

1. Diagnostics-only PCI probe, no writes beyond normal PCI enable. Done.
2. Read-only register map: named offsets backed by repeated snapshots.
3. Safe reset/status block: only writes observed in Windows traces.
4. V4L2 device registration with no streaming.
5. Single-mode DMA capture, probably 1080p60 first.
6. Resolution switching and no-signal handling.
7. ALSA audio capture.
8. DKMS packaging and install/uninstall scripts.

## Safety Rules

- Do not enable bus mastering until descriptor registers and buffer ownership are
  understood.
- Do not request IRQs by default until interrupt status and ack are known.
- Do not write MMIO offsets just because they look similar to `sc0710`.
- Keep every experimental write behind a module parameter until proven stable.
