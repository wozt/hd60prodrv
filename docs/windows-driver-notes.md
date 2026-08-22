# Windows Driver Notes

Source package:

```text
https://edge.elgato.com/egc/windows/drivers/hd60-pro/Game_Capture_HD60_Pro_1.1.0.194.exe
```

The download URL is linked from Elgato's official hardware driver page.

## INF Findings

Driver:

```text
e60MZ0380.X64.SYS
```

Supported PCI IDs:

```text
12ab:0380 subsystem 1cfa:0003  HD60 Pro Rev.1
12ab:0380 subsystem 1cfa:0005  HD60 Pro Rev.2, never released
12ab:0380 subsystem 1cfa:0006  HD60 Pro Rev.1 + Ryzen fix
12ab:0381 subsystem 1cfa:0010  HD60 Pro Rev.3
12ab:0380 subsystem 12ab:05cf  Prototype
```

Firmware files copied by the Windows driver:

```text
MZ0380.HD.HEX
MZ0380.SD.HEX
MZ0380.FW.TXT
MZ0381.HD.HEX
MZ0381.SD.HEX
MZ0381.FW.TXT
```

`MZ0380.FW.TXT` contains `01.11`; `MZ0381.FW.TXT` contains `11.11`.

## Firmware Container

The `.HEX` files are gzip-compressed tar archives, despite the extension.

Examples:

```text
MZ0380.HD.HEX -> gzip -> tar -> yuan_demo_sdi/
MZ0380.SD.HEX -> gzip -> tar -> yuan_sd/
```

The tar payload contains a small embedded Linux-style filesystem with binaries
and kernel modules, including:

```text
capture_app_infinite
video_capture_mgr
audio_capture_mgr
tinyvenc5 / tinyvenc7 / tinyvenc8
libtk_h264_encoder.so
libtk_video_capture.so
libtk_mass_mem_access.so
drivers/vpl_dmac.ko
drivers/vpl_vic.ko
drivers/vpl_edmc.ko
drivers/IICCtrl.ko
drivers/Godshand.ko
drivers/gpio_i2c.ko
```

This means the host Linux driver likely needs to download firmware to the
Mozart/MZ0380 SoC before capture can work.

## Useful Strings In e60MZ0380.X64.SYS

The Windows driver contains these relevant strings:

```text
MZ0380_SEND_COMMAND
MZ0380_DownloadBaseFirmware
MZ0380_DownloadFirmware
MZ0380_GetFirmwareVersion
MZ0380_HwInitialize
TOTAL DOWNLOAD TIMES
MST3367_HDMI_MODE_DETECT
GetHDMIDotClock
UpdateEDID
ReadEDID
VSTATE_HDCPSet
VSTATE_HDCP_Reset
FIRMWARE VERSION
BOARD VERSION
FPGA / MCU / CPLD VERSION
YUY2
H264
```

Working hypothesis:

- The host driver maps the first memory resource at `device+0x108` and the
  second memory resource at `device+0x110`.
- On the local Linux PCI resource layout, `device+0x108` corresponds to BAR0
  and `device+0x110` corresponds to BAR5.
- The Windows mailbox helper and firmware-copy paths use `device+0x108`, so
  the likely mailbox/data aperture is BAR0, not BAR5.
- BAR5 is still used by parts of the interrupt/DPC path and may be a sideband
  register block.
- The card firmware handles HDMI receiver configuration, EDID, capture pipeline,
  encoding, and possibly raw YUY2/H.264 stream production.

## Next Reverse Engineering Target

Find the mailbox protocol used by:

```text
MZ0380_SEND_COMMAND(command, ...)
MZ0380_DownloadBaseFirmware
MZ0380_DownloadFirmware
```

Until this is understood, do not write BAR registers.

## Mailbox Clues From Disassembly

Function around `0x140285074`:

```text
if flags == 0:
    base = device->mmio_at_0x108
    base[0x2c] = 0
    for i = 1; i < dword_count; i++:
        base[i * 4] = packet[i]
    base[0x00] = 0x00000800
    poll base[0x2c] bit0 for up to about 50 ms

if flags != 0:
    initialize event/semaphore at device+0x69d0
    base = device->mmio_at_0x108
    for i = 1; i < dword_count; i++:
        base[i * 4] = packet[i]
    base[0x00] = 0x00000800
    wait on device+0x69d0 with caller-provided timeout
```

Resource mapping function around `0x14028d8d1` calls `MmMapIoSpace` in a loop.
The loop stores mapped memory resources sequentially:

```text
resource 0 -> device+0x108, size at device+0x128
resource 1 -> device+0x110, size at device+0x12c
resource 2 -> device+0x118, size at device+0x130
resource 3 -> device+0x120, size at device+0x134
```

Because Linux reports only two MMIO resources for this card:

```text
BAR0: 0xfa000000-0xfbffffff, 32 MiB
BAR5: 0xfc000000-0xfc000fff, 4 KiB
```

`device+0x108` should be treated as BAR0 unless a later trace proves Windows
receives resources in a different order.

Function around `0x140277c78` is the least invasive mailbox query found so far:

```text
packet[0] = 0x00000800
packet[1] = 0x0000001c
packet[2] = selector
send_command(device, packet, 3, flags=0, timeout=0x2faf080)
if success:
    return read32(device+0x108 + 0x10)
```

The only direct call found at `0x14027a60d` passes `selector = 0xa3`. The Linux
debugfs file `fw_version` issues exactly this packet when
`allow_mailbox_writes=1`.

On the local card, issuing this command before reproducing the firmware-download
sequence made BAR5 read back as `0xffffffff`. That means the mailbox command is
real, but not valid against the device's cold state.

The base firmware path around `0x140275f64` does this after loading a firmware
blob from disk:

```text
packet[0] = 0x00000800
packet[1] = 0x0000000e
packet[2] = firmware_selector
packet[3] = blob_size
send_command(..., dword_count=4, flags=1, timeout=0x2faf080)
copy blob to device+0x108 + 0x60
packet[0] = 0x00000800
packet[1] = 0x0000000f
packet[2] = 1
send_command(..., dword_count=3, flags=1, long_timeout=0x6b49d200)
sleep 100 ms
success when read32(device+0x108 + 0x08) == 0
```

This firmware path is the next implementation target. Do not issue higher-level
mailbox commands on a cold card until this path is understood and reproduced.

The caller around `0x140247520` selects base-firmware file names by device ID,
board revision, and a step/state value. Embedded UTF-16 names include:

```text
MZ0380.LOADER.MINOR.HEX
MZ0380.LOADER.MAJOR.HEX
MZ0380.ZIMAGE.MINOR.HEX
MZ0380.ROOTFS.MINOR.HEX
MZ0380.ZIMAGE.MAJOR.HEX
MZ0380.ROOTFS.MAJOR.HEX
MZ0381.* equivalents
MZ0370.* / MZ0371.* equivalents
```

The higher-level firmware path around `0x140278f80` selects the visible package
files:

```text
MZ0380.SD.HEX / MZ0380.HD.HEX
MZ0381.SD.HEX / MZ0381.HD.HEX
MZ0370.SD.HEX / MZ0370.HD.HEX
MZ0371.SD.HEX / MZ0371.HD.HEX
```

The function around `0x1402762d4` opens the selected file with `ZwCreateFile`,
gets the file length with `ZwQueryInformationFile`, reads it with `ZwReadFile`,
then uses this mailbox sequence:

```text
packet[0] = 0x00000800
packet[1] = 0x0000000b
packet[2] = firmware_file_size
send_command(..., dword_count=3, flags=1, timeout=0x2faf080)
copy firmware bytes to device+0x108 + 0x60
packet[0] = 0x00000800
packet[1] = 0x0000000c
packet[2] = 1
send_command(..., dword_count=3, flags=1, long_timeout=0x23c34600)
sleep 100 ms
success when read32(device+0x108 + 0x08) == 0
```

`MZ0380.HD.HEX` is 936191 bytes. That cannot fit at `BAR5+0x60`, because BAR5
is only 4 KiB, but it does fit at `BAR0+0x60`, because BAR0 is 32 MiB. This is
the strongest current evidence that the firmware-copy base is BAR0.

## HwInitialize Pre-Init Sequence

Function around `0x140278bb0` performs a mailbox/IRQ pre-init before selecting
`MZ0380.HD.HEX` or `MZ0380.SD.HEX`:

```text
write32(device+0x110 + 0xdc, 2)
write32(device+0x108 + 0x30, 0)
write32(device+0x108 + 0x00, 0x400)

for up to 100 attempts:
    device->field_0xa0 = device->field_0x80 + 0x5f
    write32(device+0x110 + 0x30, low32(device->field_0x80 + 4))
    write32(device+0x110 + 0x38, low32(device->field_0xa0))
    packet = [0x800, 0x01]
    send_command(..., dword_count=2, flags=1, timeout=0x4c4b40)

read MZ0380.FW.TXT, expected local package version is 01.11

for up to 100 attempts:
    packet = [0x800, 0x0a, 0, 0]
    send_command(..., dword_count=4, flags=1, timeout=0x2faf080)
    if send succeeds and device+0x108+0x2c == 0xaaaaaaaa:
        break

sleep 100 ms
if read32(device+0x108+0x08) == expected_major and
   read32(device+0x108+0x0c) == expected_minor:
    skip visible MZ0380.HD.HEX / MZ0380.SD.HEX download
```

On the local card, `device+0x110` maps to BAR5 and those sideband references
already read as:

```text
BAR5+0x30 = 0xfa000004
BAR5+0x38 = 0xfa00005f
```

That matches local BAR0 physical base `0xfa000000` plus `0x4` and `0x5f`.

The earlier Linux visible-firmware test sent command `0x0b` against cold
hardware before this pre-init/base-firmware state was understood. That test
made PCI config and MMIO read back as `0xffffffff`, so visible firmware loading
remains blocked in the Linux driver unless explicitly forced.

Historical local Linux test result after reproducing the `0x01` pre-init before
the current reset-heavy test cycle:

```text
packet: 0x00000800 0x00000001
result: 0
completion: 0xdddddddd
irq_delta: 1
BAR0+0x08: 0x00000001
BAR0+0x0c: 0x0000000b
```

This is consistent with the async path waking by IRQ and with the already
reported firmware version matching `MZ0380.FW.TXT` (`01.11`).

Current local test result on 2026-08-22 after repeated PCI reset/recovery:

```text
attempts_requested: 100
timeout_ms_per_attempt: 2000
packet: 0x00000800 0x00000001
attempts_run: 100
result: -110
completion: 0xcc800000
irq_delta: 0
irq_count_after: 0
mailbox_030_irq_status_after: 0x00000000
```

The 2000 ms per-attempt timeout is taken from the ARM Linux driver
`MZ0380_HwInitialize`: it passes `20000000` 100 ns units to
`MZ0380_SendVendorCommand_P5` for command `0x01`, and
`MZ0380_WaitInterruptComplete` divides that by `10000` before waiting.

The same result happened with MSI/auto IRQ selection, forced legacy INTx, and
bus mastering both disabled and enabled. Public VFIO reports for `12ab:0380`
say this card is sensitive to bus/PM reset and commonly needs reset quirks such
as disabling idle D3 and avoiding bus reset. Treat the current no-IRQ preinit
failure as likely reset-state related until retested after a full host power
cycle without running `scripts/recover-device-root.sh`.

Additional Linux change/test:

```text
hd60pro_mailbox_send_async_locked now polls BAR0+0x30 for BIT(11) and runs the
same ACK sequence as the IRQ handler if the mailbox-complete status bit appears.
This emulates the Windows ISR event path even when Linux MSI/INTx delivery is
wrong.
```

Retest result: command `0x01` still ran all 100 attempts with
`result=-110`, `irq_delta=0`, `irq_count_after=0`, and
`mailbox_030_irq_status_after=0`. So the current failure is not just missed
Linux interrupt delivery; the endpoint is not raising the mailbox-complete
status bit at all in the current hardware state.

Linux now exposes a read-only debugfs snapshot:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_preinit_state
```

Current snapshot before/after command `0x01`:

```text
bar5_refs_match_windows_preinit: 1
bar0_02c_completion: 0xcc800000
bar0_030_irq_status: 0x00000000
bar5_040_payload0: 0xfa000060
bar5_048_payload2: 0xfa07085f
bar5_054_outbound_a: 0xcc800000
mailbox_irq_bit11_set: 0
```

The BAR5 preinit refs already match the Windows values, but the mailbox
completion and IRQ status never move. BAR5 still contains stale payload/outbound
state from earlier logo/DMA experiments.

Direct `0x0a` test without a successful `0x01` also timed out:

```text
packet: 0x00000800 0x0000000a 0x00000000 0x00000000
result: -110
completion: 0xcc800000
windows_marker_after_wait: 0xcc800000
marker_wait_skipped_after_async_error: 1
irq_delta: 0
status_bar_bar0_0x08: 0x00000000
status_bar_bar0_0x0c: 0x00000000
status_bar_bar0_0x10: 0x00000000
```

So the current hardware state is not selectively ignoring command `0x01`; the
early async mailbox path is silent for both `0x01` and `0x0a`.

External reset-state references:

```text
https://forums.unraid.net/topic/44969-help-passing-through-capture-card/
https://forum.level1techs.com/t/passing-through-elgato-capture-card/155610
https://www.reddit.com/r/VFIO/comments/kxhwef/passthrough_elgato_hd60_pro_capture_card_to/
```

Local Linux test result after sending the next Windows command `0x0a`:

```text
packet: 0x00000800 0x0000000a 0x00000000 0x00000000
immediate completion read: 0xdddddddd
later BAR0+0x2c: 0xaaaaaaaa
later BAR0+0x08: 0x00000001
later BAR0+0x0c: 0x0000000b
```

This matches the Windows marker and version check. The Linux implementation
must wait for `BAR0+0x2c == 0xaaaaaaaa` before reading `BAR0+0x08/0x0c`.

## Post-Version Initialization Helpers

The next `HwInitialize` region calls several compact helpers. The most relevant
ones decoded so far are:

```text
0x140287c80(line, value):
    packet = [0x800, 0x17, 1 << line, (value & 1) << line]
    send_command(..., dword_count=4, flags=0, timeout=0x2faf080)

0x140287d00(line, value):
    packet = [0x800, 0x15, 1 << line, (value & 1) << line]
    send_command(..., dword_count=4, flags=0, timeout=0x2faf080)

0x1402777e4(bus, reg):
    packet = [0x800, 0x1a, bus, reg, 0]
    send_command(..., dword_count=5, flags=0, timeout=0x2faf080)
    return read32(device+0x108+0x10)
```

The generic branch after a successful firmware-version check appears to toggle
GPIO-like lines with command `0x17`, then read HDMI/I2C-style registers with
command `0x1a`. These commands are not enabled in Linux yet; the driver exposes
`windows_init_plan` to show the decoded plan without sending it.

Historical isolated Linux test for this region was limited to:

```text
packet = [0x800, 0x17, 0x00000001, 0x00000001]
```

This corresponds to `0x140287c80(line=0, value=1)`. The old `gpio17_line0`,
`gpio17_generic_sequence`, and `i2c1a_c0_dc_db` debugfs endpoints/scripts are
now removed from the active workflow because they belong to the pre-logo replay
path, not the current firmware-owned DMAC path.

Local result:

```text
packet: [0x800, 0x17, 0x00000001, 0x00000001]
result: 0
completion: 0x00000001
BAR0+0x08: 0x00000001
BAR0+0x0c: 0x00000001
```

The card stayed accessible. The old full generic `0x17` sequence sent the eight
decoded generic-branch `0x17` packets and stopped at the first failure.

Local result for the full generic `0x17` sequence:

```text
all 8 steps result=0
all completions=0x00000001
last packet: [0x800,0x17,0x00000800,0x00000000]
card stayed accessible
```

The next Windows instructions call `0x1402777e4` twice:

```text
read bus 0xc0, reg 0xdc
read bus 0xc0, reg 0xdb
condition: first == 0x05 and second == 0x32
```

Those two `0x1a` reads are retained here as historical evidence only. They are
not part of the current Linux bring-up scripts.

## Async Wait / Recovery Path

Function around `0x14028ce04` waits on `device+0x69d0`. On timeout it normally
logs:

```text
MZ0380 COMMAND TIMEOUT( %d
```

There is a special recovery branch for command `0x06` on some board revisions.
It sends repeated command `0x1b` I2C-like writes through the non-async mailbox:

```text
[0x800, 0x1b, 0x50, 0x40, 0x00]
[0x800, 0x1b, 0x50, 0x61, 0x9f]
sleep 20 ms
[0x800, 0x1b, 0x50, 0x61, 0x1f]
sleep 20 ms
```

This recovery branch is not implemented in the Linux driver yet and should not
be run blindly.

The Linux debugfs file `firmware_load` now reproduces the visible-package path
behind explicit module parameters, but it is blocked by default because the
first hardware run on the local card showed this path is unsafe on cold
hardware:

```text
allow_firmware_load=1 allow_mailbox_writes=1 mailbox_bar=0
```

On 2026-08-15, sending only the prepare command:

```text
packet[0] = 0x00000800
packet[1] = 0x0000000b
packet[2] = 0x000e48ff
```

returned `-ETIMEDOUT`, then PCI config and both MMIO windows read back as
`0xffffffff`. The device needed recovery/power cycling. The Linux driver now
requires an additional `allow_unsafe_visible_fw_prepare=1` before this path will
send command `0x0b` again.

The disassembly also shows that Windows calls the command helper for this path
with `flags=1`, which waits on event `device+0x69d0` and relies on the IRQ/DPC
path. The first Linux attempt used a direct `+0x2c` poll, so a future retry must
first reproduce the async event/IRQ behavior more faithfully.

Before the visible firmware path, Windows runs an async pre-init loop around
`0x140278bb0`:

```text
write BAR5+0xdc = 2
write device+0x108+0x30 = 0
write device+0x108+0x00 = 0x400
write BAR5+0x30 / BAR5+0x38 with BAR0 reference values
send packet [0x800, 0x01] with flags=1
wait on event device+0x69d0
```

Linux exposes this as `preinit_command1`. It is blocked unless the module is
loaded with `allow_preinit_command1=1`.

The extracted official installer only exposes `MZ0380.HD.HEX`,
`MZ0380.SD.HEX`, `MZ0381.HD.HEX`, and `MZ0381.SD.HEX`. The base-firmware split
files were not found as standalone files in the NSIS extraction or inside the
gzip/tar payloads, so they may come from another package, a resource path, or a
fallback path not used by this installer.

Function around `0x1402851cc` builds a 5-DWORD packet used by recovery paths:

```text
packet[0] = 0x00000800
packet[1] = 0x0000001b
packet[2] = r8b
packet[3] = r9b
packet[4] = stack_byte_arg
```

The timeout recovery function around `0x14028ce04` calls this helper with values
that look like I2C writes:

```text
addr 0x50, reg 0x61, value 0x9f
addr 0x50, reg 0x61, value 0x1f
```

It waits on a kernel event stored at `device + 0x69d0`.

## Interrupt Clues From Disassembly

The interrupt path around `0x140284380` does this:

```text
status = read32(device->mmio_at_0x108 + 0x30)
write32(device->mmio_at_0x110 + 0xdc, 0x2)
write32(device->mmio_at_0x108 + 0x30, 0)
write32(device->mmio_at_0x108 + 0x00, 0x400)
if status bit 11 is set:
    KeSetEvent(device + 0x69d0)
```

The DPC path around `0x14028ed00` also reads `BAR5+0x40`, `+0x44`, `+0x48`,
`+0x4c`, and queues work based on low/mid/high byte groups in the IRQ status.

Local hardware observation complicates naming: at rest, `BAR5+0x30` read as
`0xfa000004`, which also resembles a BAR0 reference. Treat `BAR5+0x30` as
`irq_status_or_bar0_ref_a` until we have live interrupt snapshots.

Linux implementation status:

- `request_irq_vector=1` requests one IRQ vector.
- The handler reads `BAR5+0x30`, records counters, writes `BAR5+0x30 = 0`, then
  writes `BAR5+0x00 = 0x400`.
- It deliberately skips the Windows write to `device->mmio_at_0x110 + 0xdc`
  because BAR0 reads have already shown blocking behavior on this machine.

## Post-Version Local Branch And Logo Payloads

After the validated Windows-style `0x01` and `0x0a` commands, the local card
reports firmware version `01.11`, matching `MZ0380.FW.TXT`. Windows therefore
skips the visible `0x0b`/`0x0c` firmware download path on this hardware state.

The generic GPIO sequence was tested from Linux and completed successfully:

```text
[0x800,0x17,0x00000001,0x00000001]
[0x800,0x17,0x00000002,0x00000000]
[0x800,0x17,0x00000004,0x00000000]
[0x800,0x17,0x00000040,0x00000040]
[0x800,0x17,0x00000100,0x00000000]
[0x800,0x17,0x00000200,0x00000000]
[0x800,0x17,0x00000400,0x00000000]
[0x800,0x17,0x00000800,0x00000000]
```

The next two I2C-like reads were also tested:

```text
[0x800,0x1a,0xc0,0xdc,0] -> BAR0+0x10 = 0
[0x800,0x1a,0xc0,0xdb,0] -> BAR0+0x10 = 0
```

This makes the Windows condition `reg 0xdc == 0x05 && reg 0xdb == 0x32`
false, so the local path proceeds to the three logo/status payload downloads.

The three Windows functions called from `HwInitialize` are:

```text
0x140276624  MZ0380_DownloadLogoPictures1("NO.SIGNAL.BMP")
0x140276a28  MZ0380_DownloadLogoPictures2("HDCP.BMP")
0x140276dbc  MZ0380_DownloadLogoPictures3("STILL.BMP")
```

For the observed local branch, each function prepares a raw payload window with
async command `0x60`, copies `0x25800` bytes to `device+0x108 + 0x60`
(BAR0+0x60 on Linux), then sends async commit command `0x61`:

```text
selector 0x100: [0x800,0x60,0x100,0x25800,0x00f00140], copy VA 0x140319040
selector 0x200: [0x800,0x60,0x200,0x25800,0x00f00140], copy VA 0x1402f3840
selector 0x300: [0x800,0x60,0x300,0x25800,0x00f00140], copy VA 0x140319040
commit:         [0x800,0x61,1], flags=1, timeout 0x23c34600, sleep 100 ms
success check:  read32(BAR0+0x08) == 0
```

The first Linux attempt incorrectly treated `0x60` as a polling command and
timed out on `BAR0+0x2c`, but the card stayed alive and BAR5 changed to:

```text
BAR5+0x40 = BAR0 physical + 0x60
BAR5+0x48 = BAR0 physical + 0x7085f
```

The end address equals `BAR0 + 0x60 + 3 * 0x25800 - 1`, so `0x60` appears to
publish/acknowledge the whole three-logo staging window. Linux now treats that
BAR5 window as the prepare-complete condition for the selector `0x100` test.
A 5-second diagnostic wait after `0x61` showed that `BAR0+0x08` stayed at `1`,
so the Linux test now follows the Windows timing more directly: wait up to
100 ms, then report the status.

The raw payloads are not the complete external BMP files. `NO.SIGNAL.bmp` is a
640x240x32 BMP of 614456 bytes, while the device payload is 153600 bytes. The
old logo extraction/install scripts and logo upload debugfs endpoints were
removed from the active tree. This status-image replay path is now treated as
historical because it does not explain host-visible frame DMA.

## Post-Logo Command 0x1d Writes

This whole post-logo section is historical. The old Linux debugfs endpoints and
scripts named here have been removed from the active workflow because this path
did not lead to real host-visible frame DMA.

Immediately after the three logo calls, `HwInitialize` calls three larger
configuration helpers that are not fully reconstructed yet:

```text
0x14028548c(context, context+0x73a8, context+0x73b8, context+0x73bc, context+0x73c0, 1)
0x140286734(context, 0, 1)
0x140287224(context, 1, 1)
```

The local/default branch of the first helper is now partially decoded. The call
site passes `context+0x73a8` as `edx`; for the observed card this is treated as
`0`. In helper `0x14028548c`, when `context+0x73a8 < 2`, execution reaches
`0x140285435` and calls helper `0x14028658c` with:

```text
r8b = 0x9c
r9b = 0x02
stack+0x20 = 0x18
stack+0x28 = ((edx * 3) << 6) >> 7, or 0xc0 if edx >= 0x80
```

For local `edx=0`, this helper sends command `0x1b` packets:

```text
[0x800,0x1b,0x9c,0x00,0x02]  ; bank/cache update if byte at +0x2090 changed
[0x800,0x1b,0x9c,0x18,0x00]
```

The old Linux reconstruction exposed this as `post_logo_pipeline_28548c_min`,
but that endpoint is no longer registered. The next implementation work should
follow preinit/base-firmware/full-firmware/start-stream, not post-logo replay.

Further decoding showed that this file is only the small `0x14028658c` block
near the local setup code, not the whole true `0x14028548c` call-site path. For
the observed PCI config bytes (`0x0e=0`, `0x0f=0`) and `context+0x73a8=0`, the
true `0x14028548c` local path first calls `0x14024dc28`, then reaches helper
`0x1402851cc`, now decoded as a plain command `0x1b` write:

```text
[0x800,0x15,0x00000200,0x00000200]  ; line 9 = 1, sleep 50 ms
[0x800,0x15,0x00000200,0x00000000]  ; line 9 = 0, sleep 50 ms
[0x800,0x15,0x00000200,0x00000200]  ; line 9 = 1, sleep 50 ms
[0x800,0x15,0x00000100,0x00000000]  ; local line 8 = 0, sleep 50 ms
[0x800,0x1b,0x9c,0x00,0x00]
[0x800,0x1b,0x9c,0x13,0x08]
```

Linux exposes only this short head as `post_logo_pipeline_24dc28_head_local`;
the first fixed continuation slice is exposed as
`post_logo_pipeline_24dc28_table1_local`:

```text
[0x800,0x1b,0x9c,0x41,0x6f]
[0x800,0x1b,0x9c,0xb8,0x00]
[0x800,0x1b,0x9c,0x00,0x01]
[0x800,0x1b,0x9c,0x0f,0x02]
[0x800,0x1b,0x9c,0x16,0x30]
[0x800,0x1b,0x9c,0x00,0x00]
[0x800,0x1b,0x9c,0x64,0x02]
[0x800,0x1b,0x9c,0x65,0xff]
[0x800,0x1b,0x9c,0x66,0x00]
[0x800,0x1b,0x9c,0x67,0x02]
[0x800,0x1b,0x9c,0x00,0x01]
[0x800,0x1b,0x9c,0x17,0x02]
[0x800,0x1b,0x9c,0x18,0x02]
[0x800,0x1b,0x9c,0x19,0x02]
```

The next guarded continuation slice is exposed as
`post_logo_pipeline_24dc28_table2_local`:

```text
[0x800,0x1b,0x9c,0x1a,0x50]          ; bank 1
[0x800,0x1a,0x9c,0x2a,0x00]          ; read bank 1 9c:2a
[0x800,0x1b,0x9c,0x2a,read|0x07]     ; bank 1
[0x800,0x1b,0x9c,0x00,0x02]
[0x800,0x1b,0x9c,0x08,0x03]          ; bank 2
```

The next guarded continuation slice is exposed as
`post_logo_pipeline_24dc28_table3_local`. For the observed local PCI config
bytes and current context assumptions, it follows the `0x14024e085` path:

```text
[0x800,0x1b,0x9c,0x00,0x01]
[0x800,0x1b,0x9c,0x24,0x40]          ; bank 1, context+0x97bc != 1
[0x800,0x1a,0x9c,0x24,0x00]          ; if bit0 set, clear 25/26 and 27 loop
[0x800,0x1b,0x9c,0x30,0x80]
[0x800,0x1b,0x9c,0x31,0x00]
[0x800,0x1b,0x9c,0x32,0x00]
[0x800,0x1b,0x9c,0x00,0x00]
[0x800,0x1b,0x9c,0xb0,0x14]          ; bank 0
[0x800,0x1a,0x9c,0xae,0x00]
[0x800,0x1b,0x9c,0xae,read|0x04]
[0x800,0x1b,0x9c,0xad,0x05]          ; context+0x81f8 == 0 local default
[0x800,0x1b,0x9c,0xb1,0xc0]
[0x800,0x1b,0x9c,0xb2,0x00]
[0x800,0x1b,0x9c,0xb3,0x00]          ; PCI config bytes are not 5f/05
[0x800,0x1b,0x9c,0xb4,0x55]
[0x800,0x1a,0x9c,0xb4,0x00]
[0x800,0x1b,0x9c,0xb4,read&0xfc]
```

`post_logo_pipeline_24dc28_table4_local` continues the bank2 path and stops at
the next stable boundary before repeated bank0 `ab/ac` RMW and nested helper
calls:

```text
[0x800,0x1b,0x9c,0x00,0x02]
[0x800,0x1b,0x9c,0x01,0x61]
[0x800,0x1b,0x9c,0x02,0xf5]
[0x800,0x1a,0x9c,0x03,0x00]
[0x800,0x1b,0x9c,0x03,read|0x02]
[0x800,0x1b,0x9c,0x04,0x01]
[0x800,0x1b,0x9c,0x05,0x00]
[0x800,0x1b,0x9c,0x06,0x08]
[0x800,0x1b,0x9c,0x1c,0x1a]
[0x800,0x1b,0x9c,0x1d,0x00]
[0x800,0x1b,0x9c,0x1e,0x00]
[0x800,0x1b,0x9c,0x1f,0x00]
[0x800,0x1a,0x9c,0x25,0x00]
[0x800,0x1b,0x9c,0x25,read|0xa2]
[0x800,0x1a,0x9c,0x02,0x00]
[0x800,0x1b,0x9c,0x02,read|0x80]
[0x800,0x1a,0x9c,0x07,0x00]
[0x800,0x1b,0x9c,0x07,read|0x04]
[0x800,0x1b,0x9c,0x17,0xc0]
[0x800,0x1b,0x9c,0x19,0xff]
[0x800,0x1b,0x9c,0x1a,0xff]
[0x800,0x1b,0x9c,0x1b,0xfc]
[0x800,0x1b,0x9c,0x20,0x00]
[0x800,0x1a,0x9c,0x21,0x00]
[0x800,0x1b,0x9c,0x21,read&0xfc]
[0x800,0x1b,0x9c,0x22,0x26]
[0x800,0x1b,0x9c,0x27,0x00]          ; context+0x73bc == 0
[0x800,0x1a,0x9c,0x2e,0x00]
[0x800,0x1b,0x9c,0x2e,read|0xa1]
```

After this slice, the isolated `0x1402851cc` write is:

```text
[0x800,0x1b,0x88,0x03,0xa7]
```

Linux exposes this as `post_logo_pipeline_28548c_local_prefix`, guarded so it
must run after `post_logo_pipeline_28548c_min` / `24dc28_head_local` /
`24dc28_table1_local` / `24dc28_table2_local` and before
`post_logo_pipeline_287224_min`. The remainder of `0x14024dc28` is a larger
register table and is still not fully reproduced by this isolated prefix.

The local/default branch of `0x140287224` reaches
`0x14024c894` with context defaults reconstructed from the Windows init path:

```text
context+0x73a8 = 0
context+0x9920 = 0
context+0x9934 = 0
context+0x81b8 = 0
context+0x7038/0x70b8/0x7138/0x7178/0x71b8 = 0x80/0x80/0x80/0x80/0x20
```

`post_logo_shadow_probe` was added to sample the real 9c state after the
validated `24dc28` tables and the local/no-op `286734` step, before applying
the `287224/24c894` assumptions. The observed local state is:

```text
bank0: 9c:00=00 18=00 1e=00 1f=00 27=00 54=20
       ab=15 ac=95 ad=05 ce=80 cf=02 d0=00
bank2: 9c:00=02 01=61 02=f5 03=02 04=01 05=00 06=08 07=04
       09=28 17=c0 19=ff 1a=ff 1b=fc 1c=1a 1d=00 1e=00
       1f=00 20=00 21=00 22=26 25=a2 27=00 2e=a1 4a=00
```

Windows reaches this helper twice in the post-logo path being replayed here:
first from inside `0x14028548c`, and then again immediately after
`0x140286734(context, 0, 0)` at `0x14027a4d9`. The first isolated Linux
reconstruction of that helper is
`post_logo_pipeline_287224_min`. It performs only the short read/modify/write
prefix that is now decoded, and is guarded so it can run after the validated
post-logo table chain/probe or after the local `0x14028548c` GPIO tail:

```text
[0x800,0x1b,0x9c,0x00,0x02]
[0x800,0x1a,0x9c,0x4a,0x00]
[0x800,0x1b,0x9c,0x00,0x00]
[0x800,0x1a,0x9c,0xab,0x00]
[0x800,0x1b,0x9c,0xab,(read_ab & 0x95) | 0x15]
[0x800,0x1a,0x9c,0xac,0x00]
[0x800,0x1b,0x9c,0xac,(read_ac & 0xd5) | 0x15]
[0x800,0x1b,0x9c,0xad,0x07]
[0x800,0x1b,0x9c,0x1e,0x11]
[0x800,0x1b,0x9c,0x1f,0x01]
```

The `9c:ad` value was corrected from the earlier placeholder `0x00` to
`0x07`: in the local `context+0x73a8=0` branch, a zero fifth argument enters
the Windows path at `0x14024cb7b`, maps to `al=0x07`, and writes that to
`9c:ad`; `9c:1e` receives `0x11` and `9c:1f` receives `0x01`.

The large coefficient portion that follows in `0x14024c894` is not executed
by `post_logo_pipeline_287224_min`. It is exposed separately as
`post_logo_pipeline_24c894_coeffs`. For the local observed defaults
(`read_9c_4a_bank2=0`), helper `0x140274aec` is decoded as two `0x1b` writes:
it writes the high 7 bits of a coefficient to `reg+1`, then the low byte to
`reg`. The first isolated Linux coefficient set is:

```text
9b=0x103f 95=0x0000 a1=0x0000 99=0x0000
93=0x1000 9f=0x0000 9d=0x0000 97=0x0000
a3=0x1000 a5=0x2000 a7=0x0000 a9=0x2000
```

This block is guarded so it must run immediately after
`post_logo_pipeline_287224_min`.

The remaining local/default tail of `0x14028548c` is now exposed separately as
`post_logo_pipeline_28548c_tail_local`, guarded so it must run immediately
after the coefficient block. This is intentionally narrower than the full
helper: the earlier apparent writes through `0x1402851cc` are still not treated
as equivalent to the decoded `0x14028658c` wrapper. For the observed local
state (`context+0x73bc=0`, `context+0x73a8=0`, PCI class/revision bytes
`0x0e/0x0f` both `0`), the safe decoded tail candidate is:

```text
[0x800,0x1b,0x9c,0x00,0x02]
[0x800,0x1b,0x9c,0x27,0x00]
[0x800,0x15,0x00000400,0x00000400]  ; line 10 = 1
[0x800,0x15,0x00000800,0x00000000]  ; line 11 = 0
```

Then Windows sends three simple `0x1d` packets through helper `0x140287b54`:

In the root replay script, the ordering is now:

```text
... 28548c internal 286734(no-op) -> 287224_min -> 24c894_coeffs
28548c_tail_local
external 286734(no-op) -> external 287224_min -> 24c894_coeffs
A2 writes
```

```text
[0x800,0x1d,0xa2,0x11,1,low8(context+0x1d0b0)]
[0x800,0x1d,0xa2,0x12,1,0x5a]
[0x800,0x1d,0xa2,0x10,1,0x5a]
```

The context field `+0x1d0b0` is initialized from a config/registry value with
default `0`. A later property setter stores `value | 0x80000000`, but this
post-logo call uses only the low byte. The Linux diagnostic therefore assumes
`0x00` for the first write until a Windows trace proves a user override.

The helper passes a 50-second Windows timeout when its boolean flag is set, but
normal successful completions are expected to be immediate. Linux keeps the
existing short mailbox polling timeout for the first isolated test.

The next Windows block seeds the MSVC-style PRNG, generates four bytes modulo
`0xff`, then calls helper `0x140287bd8`:

```text
[0x800,0x1d,0xa2,0x13,8,<byte3:byte2:byte1:byte0>]
```

Windows then calls `0x140277c78(context, 0, 0xa3)`, which sends command `0x1c`
with selector `0xa3` and returns `BAR0+0x10` on success. Linux exposes this as
`post_logo_challenge_a2` with fixed bytes `12:34:56:78` for reproducibility.
The software check at `0x1400011f0` chooses its XOR constant once from the first
challenge byte and reuses it for four rounds; for `12:34:56:78` the expected
response is `0x70121270`. With the decoded packet order
`[selector, reg, length, value]`, the card returns this expected value.

`post_logo_challenge_sweep_a2` sends multiple deterministic challenges to
distinguish endian mistakes from a missing prior config step. Earlier sweeps
used the wrong `0x1d` packet order and are superseded by the passing
`post_logo_challenge_a2` result.

`post_logo_cmd1d_variant_sweep` then tests the unknown
`low8(context+0x1d0b0)` byte used by the first `0x140287b54` call. Candidate
values `00/01/05/5a/ff` were tested before the packet-order fix and are kept
only as historical diagnostics; rerun this sweep before drawing conclusions
from it.

Generic selector helpers decoded while chasing the remaining gap:

```text
0x1402851cc(context, 0, selector, reg, value, wait_flag)
  -> [0x800, 0x1b, selector, reg, value]

0x1402777e4(context, 0, selector, reg, len)
  -> [0x800, 0x1e, (len << 16) | (reg << 8) | selector]
```

`post_logo_selector_shadow_probe` uses the `0x1e` read form to inspect
selectors touched by `0x14028548c` (`0x30/0x88/0x94/0x9a/0xb8`). In the current
Linux sequence these reads complete, but return `0x1000` in `BAR0+0x0c` and
`0` in `BAR0+0x10`, so they are useful as command/status diagnostics rather
than a simple byte mirror of preceding `0x1b` writes.

The delay test was also run before the packet-order fix and is superseded by
the passing immediate `post_logo_challenge_a2` result. Rerun it only if later
capture setup shows timing-sensitive behavior behind `0x1c/a3`.

The currently preferred bring-up script no longer probes A3 after each partial
step. It advances the reconstructed `0x14024dc28` path through these local
blocks:

```text
table5: 0x14024e554..0x14024e84a
  bank0 ab/ac RMW
  helper 0x14024eeb8(edx=0)
  helper 0x14024d2a4
  helper 0x14024db30
  bank0 9c:51=0x89
  helper 0x14024eeb8(edx=0x30)
  bank0 9c:b7=0

table6: 0x14024d2ec head
  bank2 01=(read&0x0f)|0x60
  bank2 04=read|1
  bank2 06=8
  bank2 09=read|0x20
  bank0 54=read&0xef
  bank0 ac=read|0x80
  bank0 00=read|0x80
  bank0 ce=read|0x80
  bank0 cf=(read&0xfa)|2
  bank0 00=read&0x7f

table7 local/default: 0x14024d4db..0x14024d5b2
  assumes context+0x9810=0 and context+0x81dc=0
  table byte = {06,00,04,03,07,01}[0] = 0x06
  bank0 d0=(((table>>1)^d0)&3)^d0
  bank0 cf=((table<<7)&0xff)|(cf&0x7f)
```

Validated local continuation after that point:

```text
0x14028566b..0x14028568b:
  9a:31=1 -> BAR0+0x08 status 0x4d
  88:03=0xa7 -> BAR0+0x08 status 0x44

0x140286734:
  with observed local bytes 0x0e=0 and 0x0f=0, the decoded path returns
  without mailbox writes.

0x140287224 / 0x14024c894 split:
  287224_min ended at 9c:1f=1.
  24c894_coeffs wrote all fixed coefficient words and ended at 9c:a9=0.

0x14028548c tail:
  9c:00=2, 9c:27=0, GPIO15 line10=1, GPIO15 line11=0.
  Final GPIO line11 state reported BAR0+0x08=0x800 and BAR0+0x0c=0.

0x140287b54 A2 setup:
  a2:11=0, a2:12=0x5a, a2:10=0x5a all completed.
```

After correcting the `0x140287b54/0x140287bd8` packet order from
`[value,length]` to `[length,value]`, the deterministic A3 challenge returns
the corrected Windows software hash `0x70121270`.
The pre-`28548c` helpers at `0x140276624`, `0x140276a28`, and `0x140276dbc`
were decoded as the same three selector `0x100/0x200/0x300` status-image
uploads. They are now historical notes only.

## Current Linux Bring-Up State

The old `scripts/test-post-logo-cmd1d-root.sh` path has been removed from the
active tree. Current bring-up starts with `scripts/test-after-cold-boot-root.sh`
and then the Windows-confirmed firmware mailbox sequence.

The V4L2 node is registered as `/dev/video0` with fixed diagnostic timings:
YUYV 1920x1080 at 60 fps, Rec. 709 limited range. Until the hardware DMA ring
is decoded, `synthetic_v4l2=1` completes queued vb2 buffers with deterministic
black YUYV frames. This keeps userspace streaming paths testable without
claiming real HDMI capture.

The script also loads with `prepare_dma_buffers=1`, allocating coherent
descriptor and frame buffers. `capture_info` reports the current state:
pipeline readiness, V4L2 mode, frame counter, IRQ counters, bus-master state,
and DMA physical addresses. `real_dma_programmed` remains `0`; the next reverse
engineering target is the Windows capture-start path that writes the DMA
descriptor/ring base and enables bus mastering.

The embedded `MZ0380.HD.HEX` filesystem includes ARM modules and capture
libraries:

```text
drivers/vpl_dmac.ko
drivers/vpl_vic.ko
drivers/vpl_edmc.ko
libtk_video_capture.so.0
libvideocap.so.13
video_capture_mgr
capture_app_infinite
```

`vpl_dmac.ko` is not stripped and exports useful internal SoC DMA symbols:

```text
VPL_DMAC_SetMMRInfo
VPL_DMAC_SetupProfile
VPL_DMAC_StartHead
VPL_DMAC_StartTail
VPL_DMAC_IntrEnable
VPL_DMAC_IntrDisable
VPL_DMAC_IntrClear
VPL_DMAC_Reset
VPL_DMAC_ISRHead
VPL_DMAC_ISRTail
```

This points to the firmware handling the sensor/capture DMA internally through
VIC/EDMC/DMAC, while the host PCI driver likely communicates through the
mailbox and the PCI endpoint shared-memory/interrupt interface. The next host
driver step is therefore to locate the Windows-side endpoint/shared-memory
capture-start path, not to blindly program BAR0/BAR5 as if they were a simple
frame-grabber DMA engine.

More firmware-side strings reinforce that model:

```text
/sys/vpl_pciep/logo
/sys/vpl_pciep/epint
/sys/vpl_pciep/hready
/sys/class/vpl_pciep/channel_done

[Video_MGR][ch%d] SET_VIC fw(%d), fps(%d), resolution(%dx%d) ...
[Video_MGR]--> STOP_STREAMING ...
echo '0' > /sys/vpl_pciep/hready
killall -9 capture_app_infinite
killall -9 capture_audio_8ch
```

The firmware userland opens `/sys/vpl_pciep/epint`, writes logo buffers through
`/sys/vpl_pciep/logo`, and uses `hready`/`channel_done` as endpoint state
signals. `ep.ko` exports `pcie_set_outbound` and has `pciep_isr`,
`store_channel_done`, `pending_irqs`, `host_ready`, and `g_stream_info`
symbols. This makes the likely real Linux capture path:

1. finish the Windows mailbox pipeline until the firmware accepts host control;
2. advertise host frame/shared-memory buffers through the endpoint mechanism;
3. set the firmware-side VIC parameters equivalent to 1080p60 YUYV/HDMI;
4. enable the endpoint interrupt/channel-done path;
5. complete vb2 buffers from endpoint notifications.

## Direct-Memory Property Path

The Windows property path around `0x140246540..0x1402466f3` gives the first
concrete host-side direct-memory anchor:

```text
0x14024655c: OnGetCustomDirectMemoryModeProperty
  reads context+0x8228

0x1402465ae: OnSetCustomDirectMemoryModeProperty
  writes context+0x8228

CustomAnalogVideoDirectDMAProperty
  registry/config load at 0x140231042 stores context+0x81e4
  settings save path at 0x140262652 writes the context+0x81e4 value back

0x140246630:
  direct-memory branch is active only when context+0x8228 != 0

0x1402466af:
  copies 16 bytes from context+0x822c to the caller/output buffer

0x1402466bb:
  calls 0x14027eb38 when a stream buffer is present
```

The helper `0x14027eb38` is large. Initial reading shows it updates per-frame
metadata/timestamps and falls back to no-signal/logo image data when no real
frame is ready. This makes it a better next reverse-engineering target than
low-level BAR poking. Linux now exposes these notes as `direct_memory_info`.

The Linux driver now mirrors the decoded DirectMemory metadata shape internally
for every completed vb2 buffer:

```text
Windows per-buffer status +0x08 -> linux_last_frame_timestamp_ns
Windows per-buffer status +0x18 -> linux_last_frame_duration_ns
Windows per-buffer status +0x24 -> linux_last_frame_payload_bytes
Windows per-buffer status +0x30 -> linux_last_frame_flags
Windows per-buffer status +0x38 -> linux_last_frame_extra
Windows context+0x822c 16-byte blob -> linux_direct_memory_blob_words
```

The additional delivery callsites at `0x140293d34`, `0x140294a7a`, and
`0x14029560e` show the frame object shape passed into the same helper:

```text
frame+0x10 -> per-frame metadata/status pointer
frame+0x20 -> frame buffer pointer
frame+0x28 -> payload/stride-like value
frame+0x2c -> auxiliary pointer/value
frame+0x35 -> one-byte frame flag
```

With `synthetic_v4l2=1`, this metadata is populated from the deterministic
black-frame producer. The point is not to claim hardware capture; it makes the
future real DMA producer plug into the same state model as the Windows
DirectMemory path.

The current persistent initialization entry point is:

```sh
sudo ./scripts/load-initialized-root.sh
```

It loads the module, runs the reconstructed mailbox/post-logo/A2 sequence, and
leaves `/dev/video0` registered with coherent diagnostic DMA buffers allocated.
The module parameter `auto_init` is intentionally reported as not wired until
the debugfs sequence is refactored into side-effect helpers that can safely run
from `probe()`.

The driver also exposes `capture_start_plan`, a read-only debugfs summary of
the remaining real-capture work. It intentionally performs no hardware writes.
It tracks:

```text
DirectDMA config state: context+0x81e4
DirectMemory enable state: context+0x8228
DirectMemory 16-byte blob: context+0x822c
Firmware endpoint controls: hready, epint, channel_done
Linux candidates: coherent descriptor DMA address and frame DMA address
Missing pieces: shared-memory advertisement, SET_VIC_PARAMS mapping, frame ownership/ack protocol
```

Two Windows stream-start paths now look especially useful:

```text
0x140250d57:
  context+0x8144 = (stream_info+0x30c << 16) | stream_info+0x310
  context+0x8148 = stream_info+0x314
  context+0x814c = stream_info+0x31c
  context+0x8578 = 0x0000bb80
  context+0x72c0 = 1

0x14026edc6:
  repeats the same stream-on state after updating stream_info+0x30c/+0x310/+0x314/+0x318
```

The Linux coherent descriptor buffer is now initialized with a candidate
host-frame descriptor containing the frame DMA address, frame size, YUYV
1920x1080 format, and these decoded Windows stream-on fields. It is still only
a scaffold; the driver does not advertise the descriptor to the endpoint or
enable bus mastering.

## Firmware Endpoint Tables

`ep.ko` contains readable `.rodata` byte tables even without an ARM
disassembler:

```text
ep_cmds_size[24] = 0x0c
ep_cmds_size[25] = 0x0c
ep_cmds_size[26] = 0x10
ep_cmds_size[27] = 0x10
ep_cmds_size[28] = 0x10
ep_cmds_size[29] = 0x14
ep_cmds_size[30] = 0x2c
ep_cmds_size[31] = 0x2c
ep_cmds_size[32] = 0x2c
ep_cmds_size[33] = 0x2c
ep_cmds_size[34] = 0x2c

ep_aic_size[6] = 0x08
ep_aic_size[7] = 0x08
ep_aic_size[42] = 0x14
ep_aic_size[43] = 0x14
```

Linux exposes these as `firmware_endpoint_tables`. The next reverse step is to
map endpoint command IDs 24..34 to the firmware strings seen in `command_store`:
`BEGIN_FIRMWARE_DOWNLOAD`, `BEGIN_BASE_FIRMWARE_DOWNLOAD`, `SET_VIC_PARAMS`,
`STOP_STREAMING`, and `GET_FIRMWARE_VERSION`.

`scripts/arm-ko-disasm.py` uses Capstone/pyelftools from `.venv-re` to
disassemble ARM relocatable firmware modules. Initial `ep.ko` results:

```text
command_store:
  reads ep_command[0] as command ID
  accepts only command IDs 0x18..0x22
  copies the sysfs payload into ep_command using ep_cmds_size[cmd]
  sets ep_command+0x28 = 1

pcie_set_outbound:
  exported for firmware DMA modules
  selects one of five outbound address fields based on args
  programs endpoint registers:
    ep_regs+0x50 = 1
    ep_regs+0x54 = selected outbound high/limit word
    ep_regs+0x58 = selected outbound low/base word
    ep_regs+0x74 = 0x90000000
    ep_regs+0x7c = 0x91ffffff
    ep_regs+0xd4 = 0x00f00000

store_channel_done:
  updates endpoint registers +0x40/+0x44/+0x48/+0x4c for channel state
  accumulates pending interrupt bits in ep globals
  writes pending IRQ bits to ep_regs+0x30 when ready
```

This ties the Linux BAR5 observations to firmware: BAR5 offsets `0x40..0x4c`
are channel/status payloads, `0x30` is the firmware-side pending IRQ write, and
`0x50/+0xd4` are part of outbound window programming. The next actionable
piece is to map command IDs `0x18..0x22` to the firmware command strings.

## Embedded Endpoint ISR Decode

`yuan_demo_sdi/drivers/ep.ko` is an ARM relocatable kernel module. The local
helper `scripts/arm-ko-disasm.py` uses Capstone and pyelftools and now annotates
PC-relative literal-pool relocations, which makes the `sysfs_notify` targets
visible in `pciep_isr`.

Important endpoint-side findings:

```text
command_store accepts /sys/vpl_pciep/command IDs 0x18..0x22 only.
command_show mirrors the same `ep_command` buffer using ep_cmds_size[cmd].
pciep_isr handles additional firmware event IDs beyond command_store.
```

`pciep_isr` command `0x29` matches the firmware string:

```text
$$$ SET_VIC_PARAMS(fw %d), size(%dx%d), fps(%d)
```

The decoded payload offsets are:

```text
ep_command+0x04  flags/log gate; zero takes the SET_VIC printk path
ep_command+0x05  fps
ep_command+0x06  fw/mode; value 7 selects the epint_1080p path
ep_command+0x08  width  (u16)
ep_command+0x0a  height (u16)
ep_command+0x22  interrupt-reduce enable
```

If width or height is zero, the ISR sets the firmware `no_signal` flag and logs
`$$$ cmd(%d) => no signal`. Otherwise, it clears `no_signal` and stores mode `7`
or fallback `5` in a global used by later notification routing.

`pciep_isr` command `0x2a` is the next apparent stream-start notification step:
it skips when `no_signal` is set, updates the interrupt-reduce flag from
`ep_command+0x11`, notifies `audio_ctrl`, then notifies either `epint` or
`epint_1080p` depending on the mode selected by command `0x29`.

Useful endpoint tables extracted from `.rodata`:

```text
ep_ints_size[0x29]       = 0x28
ep_ints_size[0x2a]       = 0x14
ep_ints_1080p_size[0x29] = 0x28
ep_ints_1080p_size[0x2a] = 0x14
ep_cmds_size[0x18..0x22] = 0x0c/0x0c/0x10/0x10/0x10/0x14/0x2c...
```

Current 1080p60 candidate, not yet sent to hardware:

```text
cmd=0x29 payload_bytes=0x28 flags0=0 fps=60 fw_or_mode=7 width=1920 height=1080 interrupt_reduce=0
then cmd=0x2a payload_bytes=0x14 to trigger audio_ctrl + epint_1080p notification
```

The remaining blocker for real capture is the host transport: we still need to
find how the Windows host writes endpoint event `0x29/0x2a` or the backing
`ep_command` memory through BAR0/BAR5 after the post-logo/A2 pipeline-ready
state.

Linux now exposes this transport hypothesis directly:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/endpoint_transport_plan
```

Current model:

```text
known transport family:
  [0x800,0x60,selector,0x25800,0x00f00140]
  copy payload to BAR0+0x60
  [0x800,0x61,1]

known selectors:
  0x100 no-signal logo/status payload
  0x200 HDCP logo/status payload
  0x300 still logo/status payload

candidate capture record:
  0x2c-byte epint record with command 0x29 SET_VIC
  followed by command 0x2a post-SET_VIC/audio/epint notification
```

The key distinction is that firmware `command_store` accepts IDs `0x18..0x22`,
while `0x29/0x2a` are ISR/event IDs. That makes it more likely that capture
start is delivered through an endpoint event/interrupt record path, not the
plain `/sys/vpl_pciep/command` path. The safe next static target is any Windows
path that copies a small `0x28` or `0x2c` record into the same BAR0 staging area,
or a sibling of the logo `0x60/0x61` selector protocol.

Linux also exposes the decoded Windows staged-payload uploader:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_payload_uploader
```

The main helper at `0x140277150` is more general than the three named logo
siblings, but it still validates an image-like blob:

```text
requires magic 0x55aa55aa
requires width <= 0x140 and height <= 0x0f0
copies payload to context+0x108+0x60, which matches Linux BAR0+0x60
prepare: [0x800,0x60,selector << 16,size,(height << 16) | width]
commit:  [0x800,0x61,1]
```

The only direct callsite in the current objdump is `0x140244e10`. That caller
copies 16-byte property/context blobs to `context+0x1a8d4`,
`context+0x1a8e4`, and `context+0x1a8f4`, builds a `0x208`-byte payload via
`0x14027596c`, reads the selector from `context+0x1c944`, and then calls
`0x140277150`. This looks like a dynamic property/still-logo upload path, not
the DMA stream-start path.

That gives a useful negative result for capture bring-up: `0x60/0x61` is a
confirmed host-to-firmware staging primitive, but this decoded helper is not
the raw `0x2c` `SET_VIC` event transport. The next static target remains either
a non-image caller of this helper, a sibling uploader without the
`0x55aa55aa`/size checks, or a direct write path into the endpoint event memory
behind `/sys/vpl_pciep/epint`.

## Windows Stream State Flow

Linux now exposes the decoded Windows stream-state flow:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_state_flow
```

Three Windows paths now line up:

```text
0x140250cfe..0x140250d57
  writes context+0x727c, +0x8144, +0x8148, +0x814c, +0x8578,
  then context+0x72c0 = 1

0x14026dec0
  stream/status worker; iterates sibling/channel contexts, attaches
  context+0x1f1a8/+0x1f1b0, updates chip 0x88 signal/video state, and repeats
  the same stream-state write at 0x14026ed84..0x14026edc6

0x140283462..0x14028358a
  format/property path; fills stream_info+0x30c/+0x310/+0x314/+0x31c and
  derived fields, then writes the same context stream state
```

For the local 1080p60 model this matches the Linux host descriptor scaffold:

```text
context+0x8144 = 0x07800438
context+0x8148 = 0x0000003c
context+0x814c = 0x00000000
context+0x8578 = 0x0000bb80
context+0x72c0 = 1
```

This is not yet DMA programming. It is Windows host context state plus signal
conditioning, but it gives the next xref target: consumers of
`context+0x72c0`/`context+0x8144` that advertise host buffers, arm endpoint
capture, or feed real frame delivery.

Linux also exposes the decoded consumers of those stream-state fields:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_consumers
```

Current consumer map:

```text
0x14023eb70 family
  KS-style getters/setters for +0x8144/+0x8148/+0x814c and derived format
  values.

0x14022e540
  query path that returns context+0x72c0 streaming state to userspace.

0x140218c98 and 0x14021a473
  format builders consuming +0x8144/+0x8148/+0x814c/+0x727c, then calling
  0x14021ee5c and 0x140228918.

0x14027e7b0 / 0x14027f073 / 0x14027eb38
  DirectMemory/frame-delivery path. These use stream-state fields for timing,
  metadata, frame counters, and synthetic/no-signal frame delivery.

reset paths
  0x140250e82, 0x140253d5f, 0x14026e14c, and 0x1402835d8 clear the same
  fields and reset context+0x72c0.
```

This explains the V4L2/DirectMemory metadata side, but it is still a negative
result for real DMA: the buffer advertisement and endpoint ring programming
must be in a different user of the stream context, likely below
`0x14021ee5c`/`0x140228918` or a path touching `+0x81e4` and endpoint counters
`+0x204c/+0x2050/+0x2058/+0x205c`.

## Windows Buffer Queue

Linux exposes the decoded Windows software frame-buffer queue:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_buffer_queue_info
```

The queue creation path at `0x14021e190` is called from the format builders
after `0x14021ee5c`. It allocates a `0x140`-byte object with pool tag
`0x48504a59`, links it into the eight-entry context queue
`+0x6d58..+0x6d90`, and allocates:

```text
queue+0xf0 = 0x0c00-byte buffer
queue+0xf8 = 0x0c00-byte buffer
queue+0x108 = format-size-dependent video buffer
```

The format-dependent allocation accepts size constants `0x4380`, `0x4920`,
`0x5a00`, `0x5fa0`, and `0x6540` from `format+0x08`. The frame helper around
`0x14027e0db..0x14027e399` later uses `queue+0xd0` as a frame base and copies
from planes at `+0x4000/+0x8000/+0xc000`, or from external stream buffers at
`+0x2c8/+0x2d4`.

This is closer to the capture path, but it is still host-side software storage.
The missing producer side is now narrower: find who fills `queue+0xd0` or the
external buffers, and where that path writes endpoint/outbound registers or
shared-memory descriptors that the firmware can DMA into.

## Windows Frame Producer Search

Linux exposes the current producer/consumer split as:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_frame_producer_search
```

The latest static pass separated several useful negative results from the
remaining true hardware path:

```text
0x140230000..0x140230046
  zeroes context queue/table families:
  +0x6b58, +0x6a98, +0x6cd8, +0x6d58, +0x6dd8, +0x6e58
  This is initialization/reset, not a frame producer.

0x14021e190 / 0x14021e480
  create and destroy the 0x140-byte host queue objects linked into
  context+0x6d58..+0x6d90.

0x14027e311..0x14027e361
  reads stream object +0x2c8 as source base and +0x2d4 as source size, then
  copies that already-visible memory through helper 0x140297500.

0x14027e3b0..0x14027e8xx
  DirectMemory metadata/timing code using stream_info+0x58/+0x40/+0x48,
  per-stream counters +0x9c/+0xdc/+0x15c/+0x19c, and context counters
  +0x97f0/+0x98c0/+0x98c4.
```

So far, these paths consume or wrap memory that is already visible to the
Windows host. They do not advertise a Linux host physical address to the
firmware, program BAR5 outbound windows, enable bus mastering, or arm a DMA
ring.

The next static targets are write xrefs to stream object `+0x2c8/+0x2d4`,
queue `+0xd0`, DirectDMA config `context+0x81e4`, and endpoint counter/status
fields `+0x204c/+0x2050/+0x2058/+0x205c`.

## Windows External Stream Buffer

Linux exposes the decoded `stream object +0x2c8/+0x2d4` lifecycle:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_external_buffer_info
```

The live stream object allocator around `0x14027a2d6` allocates several large
host buffers:

```text
stream+0x1f8/+0x200/+0x208/+0x210: 0x00655000-byte buffers
stream+0x2a8/+0x2b0/+0x2b8/+0x2c0/+0x2c8: 0x001d4c00-byte buffers
```

The final allocation is stored at `stream+0x2c8` at `0x14027a372`. The cleanup
path at `0x1402824e1` frees `stream+0x2c8` and clears it together with the
neighboring `+0x2a8..+0x2c0` buffers. The runtime preparation path at
`0x14028ac57` clears `stream+0x2c8` for `0x001d4c00` bytes before recomputing
`stream+0x2d4` around `0x14028acef`.

The many writes to `rbp+0x2c8/+0x2d4` in `0x14021f843..0x14022bb24` are mostly
stack/local format templates, not the live stream object. Several of these
templates use `+0x2d4 = 0x00100000`, which explains why the static search
initially looked like a DMA ring size. It is still a software buffer template
unless a later path advertises the address to the endpoint.

This narrows the real missing producer: Windows has host memory buffers, and
the delivery path consumes them. The remaining target is the call that tells
firmware/endpoint hardware where those buffers are, likely through host
physical address translation, BAR5 outbound-window programming, or a BAR0+0x60
staged payload that carries host addresses rather than logo data.

## Windows DMA Mapping Init

Linux exposes the decoded PCI/resource/DMA initialization anchors:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_dma_mapping_info
```

The import table resolves the key indirect calls:

```text
0x1402a5068 -> KsDeviceGetBusData
0x1402a5060 -> KsDeviceRegisterAdapterObject
0x1402a5190 -> MmMapIoSpace
0x1402a51f0 -> MmAllocateContiguousMemorySpecifyCache
0x1402a51f8 -> MmFreeContiguousMemorySpecifyCache
0x1402a5210 -> IoGetDmaAdapter
0x1402a5230 -> MmGetPhysicalAddress
```

Around `0x14028d553`, Windows reads PCI/config data with `KsDeviceGetBusData`.
Around `0x14028d8d1`, it maps translated PCI resources using `MmMapIoSpace`
into sequential device slots; `device+0x108` still matches BAR0 on the Linux
card, and the next mapped slot is likely BAR5.

The first contiguous allocation at `0x14028d628` is only a 0x1000-byte probe:
Windows allocates contiguous memory, calls `MmGetPhysicalAddress`, logs/records
the physical address, then immediately frees it with
`MmFreeContiguousMemorySpecifyCache`.

The persistent DMA-relevant allocations are later:

```text
0x14028d83d:
  IoGetDmaAdapter -> device+0x40
  map-register count/result -> device+0x48

0x14028e19c:
  contiguous buffer -> device+0xd0
  physical address  -> device+0xc8

0x14028e2a2:
  contiguous buffer -> device+0x140
  physical address  -> device+0x138

0x14028e4db / 0x14028e5c3:
  per-channel contiguous buffers -> device+0x1190-family
  physical addresses             -> device+0x190-family
```

Then `0x14028e2ee` calls `KsDeviceRegisterAdapterObject` with `device+0x40`,
`0xfffffff8`, and `0x10`. This is the first clearly confirmed Windows path
that owns physical-address-backed buffers. It is now a stronger real-DMA target
than `stream+0x2c8`, which is just host delivery memory.

The remaining missing edge is still explicit address advertisement: find the
later xrefs that carry `device+0xc8`, `device+0x138`, or the `device+0x190`
physical address family into BAR0/BAR5 or a firmware endpoint payload.

## Firmware Userland Event Loop

`video_capture_mgr` is a stripped ARM executable, but the Capstone helper can
disassemble its `.text` and annotate `.rodata` strings and PLT calls.

The main event loop opens `/sys/vpl_pciep/epint` with read/write access, reads a
0x2c-byte record, polls forever, then uses `pread(fd, event, 0x2c, 0)` and
dispatches on `event[0]`. Several handled paths call `pwrite(fd, event, 0x2c, 0)`
after processing, which strongly suggests a 0x2c-byte event/ack ownership
protocol between `ep.ko` and `video_capture_mgr`.

Decoded userland branches:

```text
event 0x29  SET_VIC path; validates width/height > 0x7f, stores per-channel capture config, starts tinyvenc path
event 0x2a  Set AIC audio params path
event 0x60  LOGO BEGIN DOWNLOAD path; saves channel/logo type/firmware size/logo order, then acks
event 0x61  LOGO END DOWNLOAD path; finalizes logo state, then acks
event 0x6e  LOAD_FILES path; may transform payload bytes, then acks
event 0x07  STOP_STREAMING path; runs echo '0' > /sys/vpl_pciep/hready and kills capture_app_infinite/capture_audio_8ch
```

The `SET_VIC` userland format string is richer than the ISR log:

```text
[Video_MGR][ch%d] SET_VIC fw(%d), fps(%d), resolution(%dx%d) interlace(%d), m(%d), color_info ..., input_frame_width(%d), input_frame_height(%d), bitstream_num(%d), ... is_nosg(%d)
```

This reinforces the current model: the Linux host must either reproduce the
host-side event transport that makes firmware/userland see 0x29/0x2a records, or
find the shared-memory region behind `/sys/vpl_pciep/epint` and advertise host
buffers there. Directly toggling A3/version queries will not start capture.

## Windows 0x88 Bridge Helpers

The Windows helper at `0x1402777e4` builds a normal mailbox read packet:

```text
[0x800, 0x1a, chip, reg, 0]
```

It returns the low byte from `BAR0+0x10` after `MZ0380_SEND_COMMAND`.

The paired helper at `0x1402851cc` builds a normal mailbox write packet:

```text
[0x800, 0x1b, chip, reg, value]
```

Stream-start and resolution-update paths use these helpers against chip `0x88`,
especially registers `0x15`, `0x16`, and `0x18`. The helper at `0x14026edd8`
also reads `0x88:40` and a resolution-dependent range around `0x88:8d..0x95`
into an 8-byte buffer.

The stream-update block at `0x14026ec85..0x14026ed36` does this:

```text
read 0x88:18
read 0x88:16
read 0x88:15
base_phase = ((read15 & 0x70) << 4) | read16
base_gain = read18
adjusted_phase = base_phase + context->calibration_36c[index]
adjusted_gain = base_gain + context->calibration_a20[index]
write 0x88:15 = ((adjusted_phase >> 4) & 0x70) | (read15 & 0x0f)
write 0x88:16 = adjusted_phase & 0xff
write 0x88:18 = adjusted_gain & 0xff
```

Historical note: Linux previously exposed the decoded plan and live source
reads at the now-removed debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_88_update_plan
```

The calibration property paths that feed `context+0x36c` and `context+0xa20`
were previously summarized at the now-removed debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_calibration_info
```

Important decoded paths:

```text
0x14023f0a0: get phase calibration from stream_info[index]+0x36c
             or alternate stream_info[index]+0x1e3c
0x14023f120: set phase calibration, mark dirty flags, update 0x9c:80/81,
             and when the endpoint bridge exists update 0x88:15/16
0x14023f320: get gain calibration from stream_info[index]+0xa20
             or alternate stream_info[index]+0x1ea4
0x14023f3a0: set gain calibration, mark dirty flags, update 0x9c:84,
             and when the endpoint bridge exists update 0x88:18
0x1402518aa: reads stream_info[index]+0x36c during setup
0x1402518b2: reads stream_info[index]+0xa20 during setup
```

The live Linux card currently reads `0x88:15`, `0x88:16`, and `0x88:18` as
zero after the pipeline-ready init. Do not enable writes to these registers
until the default calibration values are recovered from Windows initialization
or from a trace.

## Windows Bridge Attach

Windows stores a parent/controller context at `context+0x1f1a8` and a channel
index at `context+0x1f1b0`. This is used by the `0x88` register update helpers,
but the pointer itself is not a host DMA buffer.

Historical note: Linux previously summarized the decoded attach paths and live
read-only `0x60` status probe at the now-removed debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_bridge_attach_info
```

Important decoded paths:

```text
0x140254614: iterates sibling contexts from global table 0x14039f160 using
             context+0x1f190; stores sibling+0x1f1a8=parent and
             sibling+0x1f1b0=channel index
0x14026dec0: stream/status worker repeats the same attach for active channels
0x1402441a0: property/query path calls 0x14026edd8 through +0x1f1a8/+0x1f1b0
0x140244210: property/query path calls 0x14026fa98 through +0x1f1a8/+0x1f1b0
```

After attaching, `0x140254614` selects `0x60:ff = (channel & 3) + 5`, reads
`0x60:f0/f4/f5` and sometimes `0x60:f3/f2`, checks `0x60:5c`, and may toggle
helper/GPIO lines `0x12` and `0x24` with long sleeps when the state resolves to
`0x31` or `0x32`.

## Windows 0x88 Capture Presets

Historical note: Linux previously documented the write-only plan, without
applying it, at the now-removed debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_capture_88_presets
```

The dispatcher around `0x140272a0f` selects chip `0x88` preset helpers based on
`stream_info+0x2080`. Stream IDs seen in this path include `0x2400`, `0x2601`,
`0x2611`, `0x260c`, and `0x270c`.

Common entry work:

```text
if stream_info+0x2080 >= 0x2200:
    write 0x88:35 = 0x05
read-modify-write 0x88:02 = (old & 0xfa) | 0x02
read-modify-write 0x88:f5 using mask table byte at 0x1402d5afc[index]
```

The first recovered `0x1402d5afc` mask bytes are:

```text
fe fd fb f7 f0 00 00 00
```

Two nearby preset helpers write the same fixed sequence, except register
`0x39`:

```text
0x140272d98: 0x39 = 0x8c
0x140272f1c: 0x39 = 0x88
fixed registers: 0c=03 0d=10 20=60 26=02 2b=58 2d=30 2e=70
                 30=48 31=bb 32=2e 33=90 2c=0a 27=2d 28=00 13=00
final: read 0x88:14 and write old & 0x9f
```

There is also a special tail around `0x140272bc3` for a mode-byte-2 path and
`0x2601`-family/`0x270c` stream IDs:

```text
16=40 15=13 16=0a 17=00 18=19 19=d0 1a=25 1c=06 1d=7a
```

Historical note: Linux previously had an opt-in minimal applicator for the
`0x2400` preset at the now-removed debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/apply_capture_88_preset_2400_min
```

It required the now-removed `allow_capture_88_writes=1` parameter and
deliberately skipped the unknown `0x88:f5` mask-table write and the
`0x15..0x1d` 1080p-like tail. On the local HD60 Pro test, every write returned
mailbox completion `1`, but immediate `0x1a` reads of the same `0x88`
registers still returned zero. Treat that as command acceptance only, not proof
that the target register bank latched the values.

Historical note: Linux also previously exposed the post-preset dynamic `0x88`
table decode without touching hardware at the now-removed debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_88_mask_tables
```

This covers Windows helpers `0x14027383c` and `0x1402739ac`. Both select chip
`0x88` registers from table `0x1402d5984`, apply AND masks from `0x1402d59f4`,
and the set helper ORs bytes from `0x1402d5af0`.

Recovered first table bytes:

```text
regs:  fa fa fb fb 00 00 00 00 01 00 00 00 02 00 00 00
masks: f8 8f f8 8f 00 ff ff ff ff ff ff 00 02 03 00 00
or:    01 10 01 10 01 02 04 08 0f 00 00 00 fe fd fb f7
```

For stream IDs `0x2400`, `0x2611`, and `0x270c`, Windows can use four
consecutive slots when the selector is >= 4. For `0x2601` and `0x260c`, it uses
two slots with even indexing. For smaller selectors, the helper uses either the
direct selector or `(selector & 1) * 2` depending on stream family.

Related dynamic helpers:

```text
0x140272d20: writes count bytes from caller table to consecutive 0x88
              registers starting at base dl
0x1402737d4: writes 0x88:40 with a Windows channel/mode selector mapping
0x1402734d8: encodes one byte into three consecutive 0x88 writes
0x14026fa98: consumes an 8-byte channel state buffer and writes dynamic 0x88
              windows around 0x56..0x80, gated by stream_info+0x2080
```

Two `0x140272d20` callsites feed literal `.rdata` tables that look like
GUID/property data but are consumed as chip `0x88` payload bytes:

```text
0x14027282e: table 0x14033f770, base 0x15, count 9
0x140272a74: table 0x14033f7a0, base 0x15, count 9

first nine written bytes for both:
88:15=e9 88:16=4b 88:17=81 88:18=6f 88:19=f6
88:1a=9a 88:1b=cf 88:1c=43 88:1d=92
```

The missing input is the exact live channel/state selector passed into these
helpers after the Linux init sequence. Until that is recovered, these dynamic
updates should remain documentation rather than automatic writes.

Linux now exposes a guarded read-only debugfs probe:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/endpoint_bridge_regs
```

This does not write configuration values to the `0x88` block, but it does issue
mailbox read commands, so it remains behind `allow_mailbox_writes=1` and
`allow_i2c_read_command1a=1`.

## Candidate SET_VIC Event Record

Linux now exposes the current 1080p60 `SET_VIC` record model without sending it:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/set_vic_event_record
```

Current 0x2c-byte candidate:

```text
offset 0x00: command 0x29
offset 0x04: channel 0
offset 0x05: fps 60
offset 0x06: fw/mode 7
offset 0x07: interlace 0
offset 0x08: width 1920
offset 0x0a: height 1080
offset 0x18: input_frame_width model 1920
offset 0x1a: input_frame_height model 1080
offset 0x1c: bitstream_count model 1
```

The first offsets are confirmed both by `ep.ko:pciep_isr` and
`video_capture_mgr`. The later input-frame/bitstream offsets come from the
`video_capture_mgr` format-string argument order and should be treated as a
model until a Windows host trace confirms them.

## Persistent Physical Buffers

The Windows PCI/DMA init block `0x14028d250..0x14028e9ee` creates physical
address backed allocations that are more relevant to real capture than the
later DirectMemory software buffers.

Confirmed xrefs:

```text
0x14028e19c: allocates device+0xd0, size from device+0xe0
0x14028e1b7: stores MmGetPhysicalAddress(device+0xd0) at device+0xc8
0x14028e2a2: allocates device+0x140, size from device+0x148
0x14028e2bd: stores MmGetPhysicalAddress(device+0x140) at device+0x138
0x14028e4db: allocates per-channel buffers at device+0x1190 + index*8
0x14028e522: stores per-channel physical addresses at device+0x190 + index*0x10
0x14028e52f: stores per-channel size/metadata at device+0x198 + index*0x10
0x14028e81c: cleanup path for device+0xd0/device+0xc8/device+0xe0
0x14028e8a6: cleanup path for device+0x140/device+0x148
0x14028e8f3: cleanup path for device+0x1190/device+0x190/device+0x198 families
```

Later `0x1190` consumers are host-side frame buffer selection/copy paths:

```text
0x14027b977: selects a device+0x1190-family host pointer by channel/slot
0x14027fb79: selects a device+0x1190-family host pointer by modulo frame slot
0x14028026f: reads these buffers and interleaves/copies byte/word planes
```

So `device+0x1190` is useful for modeling the Windows frame memory layout, but
these consumers still do not show the initial endpoint outbound-window or DMA
advertisement step.

Linux now mirrors this layout with extra coherent allocations when
`prepare_dma_buffers=1`, exposed through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/dma_info
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_physical_buffer_xrefs
```

These addresses are not advertised to hardware yet. The missing piece remains
the Windows path that consumes `device+0xc8`, `device+0x138`, or the
`device+0x190` physical-address family and passes them to BAR0/BAR5, a DMA
adapter callback, or the firmware endpoint event transport.

Important correction: calls through `0x1402a52d0` are not evidence of a DMA
adapter callback. The import table ends earlier, and the qword at
`0x1402a52d0` points at `0x1402974c0`, which is just `jmp rax`. Treat this as
Windows CFG/guard indirect-call dispatch noise unless the surrounding target
is independently recovered.

## BAR Mapping Xrefs

The mapped resource model is now:

```text
device+0x108: first mapped memory resource, matching Linux BAR0
device+0x110: second mapped memory resource, matching Linux BAR5
```

Relevant Windows xrefs:

```text
0x140278c0c: preinit pattern, BAR5+0xdc=2 then BAR0+0x30=0 and BAR0+0=0x400
0x140278c8e: writes BAR5+0x30 and BAR5+0x38 from device payload-window fields
0x1402843b8: repeats the BAR5+0xdc/BAR0+0x30/BAR0+0 sequence in worker context
0x14028444a: reads BAR0+0x40/+0x44/+0x48/+0x4c interrupt payload bytes
0x140288303: clears BAR0+0x50/+0x54/+0x58/+0x5c after a stream state gate
```

The early `0x14021d8ac`-style `+0x108/+0x110` references are offset
collisions on queue/format objects, not the device BAR slots. Linux exposes
the BAR xref model through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_bar_mapping_xrefs
```

## Worker Event Path

The path around `0x14028adf0` creates a Windows worker callback at
`0x140284380`. This is now decoded as an endpoint-event consumer:

```text
0x14028adf0: stores callback 0x140284380 and starts a worker object
0x14028ae74: stops the worker and marks device+0x7699
0x140284523: worker loop exits when device+0x7698 becomes nonzero
0x1402843b8: reads BAR0+0x30, acks via BAR5+0xdc/BAR0+0x30/BAR0+0
0x14028444a: reads BAR0+0x40/+0x44/+0x48 and queues a 24-bit token
0x1402844c5: reads BAR0+0x4c when high payload bits are present
0x14028eccd: sibling drain path reads BAR0+0x40-family and clears BAR0+0x50
```

The queue target is `device+0x29b0 + slot*0x40`, using guarded helper
`0x1402a51b0` while holding/using the lock-like object at `device+0x69b8`.
The timer-like sibling at `0x14028294b` uses the same helper for software event
tokens, so this helper is queue insertion/collection logic, not DMA
programming.

This is useful because it ties BAR0+0x40..0x4c to endpoint event payloads and
the firmware `pciep_isr/store_channel_done` model. It is still a consumer path:
the missing capture-start piece is earlier, where Windows advertises host frame
buffers and causes the endpoint to start producing these events.

Linux exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_worker_event_path
```

`device+0x29b0` is now modeled separately:

```text
0x14028e731: initializes 0x100 queue entries, stride 0x40, callback 0x14028ee50
0x14028444a: queues BAR0+0x40/+0x44/+0x48 low payload tokens
0x1402844c5: queues BAR0+0x4c high payload tokens
0x14028ed61/0x14028ed9b/0x14028edd5: sibling event-drain insertion paths
```

This appears to be a 256-slot endpoint-event collection. The frame delivery
path at `0x140293c39` locks `device+0x1d108` and calls
`0x14027eb38/0x14027e3b0/0x14027d698`, so the event queue is not itself the
frame buffer or physical DMA ring. The useful next static target is callback
`0x14028ee50` and the handoff from this event queue to `device+0x1d108` stream
objects.

Linux exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_event_queue_xrefs
```

The queue callback `0x14028ee50` is now decoded as the bridge between endpoint
events and the Windows stream/frame software objects:

```text
0x14028ee50:
  rdx = device context
  r8  = event/token mask
  r9  = callback payload/context
  locks device+0x69b8
  builds temporary arrays of list heads, frame pointers, metadata pointers,
  flags, and output slots from the 0x29b0 queue objects
  calls stream_object+0x10 via CFG dispatch 0x1402a52d0
  writes callback-returned slot values back into queue/list objects
  signals wait/event objects at slot+0x100
```

This gives a plausible software path:

```text
BAR0+0x40..0x4c endpoint payload
  -> device+0x29b0 queue slot
  -> 0x14028ee50 callback bridge
  -> stream_object+0x10 function pointer
  -> device+0x1d108 frame delivery lists
  -> DirectMemory frame helpers
```

The stream object is allocated at `0x1402381dd` with size `0x24f0` and stored at
`device+0x1d110`; `device+0x1d108` and `device+0x1d100` are lock/list-head
objects initialized during device setup. This is still not the hardware DMA
programming point: the remaining critical target is the writer of
`stream_object+0x10`, because that function pointer is the handoff that consumes
the callback arrays and likely maps endpoint events into real frame objects.

Linux exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_event_callback_bridge
```

The writer of `stream_object+0x10` remains the critical missing link. The
current decoded state is:

```text
0x14028f4c9:
  0x14028ee50 loads stream_object+0x10 and calls it through CFG dispatch
  0x1402a52d0 after preparing endpoint event arrays

0x1402381c5..0x1402381f6:
  allocates a 0x24f0-byte stream object, stores it at device+0x1d110,
  zeroes it, and then initializes stream fields

0x14022d0c8:
  bounded string formatter around 0x140295898; not the callback installer

0x14024a768 sampled call family:
  reads video/I2C timing registers and computes format timing; not the direct
  stream+0x10 writer in the currently decoded slice
```

There is no simple direct immediate store to `[stream+0x10]` in the sampled
disassembly. The current working model is that this callback is installed
indirectly through a helper or nested table during stream object/property
construction. The next static targets are the tail of the stream constructor,
all callees receiving `r14` or `r14+0x1ea4`, and the selected globals/tables
around `0x140340ac0..0x140340ad8` and `.rdata 0x1402a0c00/0x1402a0c30`.

The global selector branch immediately before stream allocation is now partially
excluded as a callback source. The selected backing tables at
`0x14033ec50`, `0x14033ed10`, and `0x14033edd0` decode as KS/audio-format
descriptor data: they contain `auds`, GUIDs, sizes, and format timing/range
values, not executable function pointers. The property handler family around
`0x14023e760..0x140241220` repeatedly recovers the device context through
`0x1402a50a8/0x1402a50a0` and reads/writes stream/device fields such as
`+0x72c0`, `+0x73b0`, `+0x8140`, `+0x8158..+0x8170`, `+0x1ea4`, `+0x1f0c`,
and `+0x1fdc`; those handlers are format/routing state and I2C tuning helpers,
not the stream callback installer.

Linux exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_callback_search
```

## DirectMemory Counter Fields

The `stream_info+0x204c/+0x2050/+0x2054/+0x2058/+0x205c` family is now decoded
as a software cadence/drop/reset gate around DirectMemory frame delivery:

```text
0x140253d59: stores fps/2 into stream_info+0x2050 during format/reset
0x140253f7c: repeats the +0x2050 update after stream/pipeline changes
0x14027b374: clears external-buffer state and +0x2054/+0x205c when idle
0x14027db98: clears +0x205c in DirectMemory timing path
0x14027ef02: clears +0x2058 in an alternate timing path
0x14027f1f7: decrements +0x2050 during DirectMemory frame delivery
0x140289447: teardown clears qword families at +0x2050 and +0x2058
```

This is another useful negative result: these fields are not PCI BAR registers
and do not appear to be the firmware `channel_done` ownership protocol. Linux
exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_frame_counter_info
```

The DirectMemory drain path is now separated from the missing PCI endpoint DMA
programming path:

```text
0x140293c39:
  locks device+0x1d108
  pops frame/list objects
  resets frame metadata at frame+0x10
  dispatches by stream/frame type to 0x14027eb38, 0x14027e3b0, or 0x14027d698

0x14027eb38:
  consumes device+0x1d110 stream fields
  reads software source data from device+0xd0 + offsets such as
  0x0/0x2000/0x4000/0x6000
  copies blocks into the delivered frame buffer with 0x140001820
  updates timestamps/payload metadata

0x140294c09 / 0x140294e0d:
  load object->+0xc0->+0x10
  map frame type to a slot 0..0x1e
  call 0x14028fa04 state updater
```

This looks like software frame delivery or no-signal/format fallback from
Windows contiguous memory, not the physical host-frame advertisement to the PCI
endpoint. There are no decoded BAR0/BAR5 writes or physical address publications
inside this drain path. Linux exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_directmemory_drain_path
```

## Windows Contiguous Buffer Layout

The `0x14028d250` initialization block now gives a clearer host-buffer layout
than the earlier DirectMemory drain path:

```text
control buffer:
  device+0xe0 size
  device+0xd0 virtual address
  device+0xc8 physical address

status buffer:
  device+0x148 size
  device+0x140 virtual address
  device+0x138 physical address

channel buffers:
  device+0x1190 + slot*8     virtual address
  device+0x190  + slot*0x10  physical address
  device+0x198  + slot*0x10  size/metadata
  device+0x2590 + group*0x80 + slot*4 software state
  device+0x2990 + group*4    cleanup size table
```

The allocation stores happen at `0x14028e19c/0x14028e1b7`,
`0x14028e2a2/0x14028e2bd`, and `0x14028e4db/0x14028e510/0x14028e52f`,
with fallback paths at `0x14028e20c`, `0x14028e3f2`, and
`0x14028e5c3/0x14028e5f5/0x14028e611`. Cleanup mirrors those fields around
`0x14028e81c..0x14028ebc1`.

This is the closest current match to Windows host physical buffers, but it is
still not the endpoint advertisement. This constructor prepares physical
addresses; it does not write them to BAR0/BAR5 or to an identified firmware
event record. Linux exposes the current mirror state through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_contiguous_buffer_layout
```

## DMA Publish Search

Two offset families have to stay separated:

```text
device context, r14 in 0x14028d250:
  +0xc8/+0xd0/+0xe0 and +0x138/+0x140/+0x148 are real contiguous DMA
  physical/virtual/size fields.

stream/queue config object, allocated at 0x14021e190:
  +0xc0/+0xc8/+0xd0 copy host object pointers into a 0x140-byte queue/config
  object inserted into device+0x6d58..+0x6d90.
```

The `0x14021e190` family is therefore a useful false positive: it explains why
`+0xd0` appears in DirectMemory software-copy paths, but it is not the missing
PCI endpoint address publication. The driver now exposes this distinction in:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_dma_publish_search
```

## Firmware PCIe Outbound Registers

`yuan_demo_sdi/drivers/ep.ko` is an ARM32 relocatable module with symbols. The
host does not currently have an ARM disassembler installed, but `readelf` plus
the raw `.text` bytes identifies `pcie_set_outbound` at `.text+0x5a8`, size
`0xa0`.

Manual ARM immediate decoding of the function shows writes to the endpoint
channel window:

```text
selected_channel+0x50 = 0x00000001
selected_channel+0x74 = 0x90000000
selected_channel+0x7c = 0x91ffffff
selected_channel+0x54 = caller arg0
selected_channel+0x58 = caller arg1
selected_channel+0xd4 = 0x0f000000
```

That is now the strongest firmware-side hint for the missing outbound/DMA
advertisement path. Linux exposes BAR5 readback and the decoded constants in:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/firmware_pcie_outbound_regs
```

`vpl_dmac.ko` imports `pcie_set_outbound`. Relocations show the two direct
callers:

```text
VPL_DMAC_StartTail: .rel.text offset 0x00000c94 -> pcie_set_outbound
VPL_DMAC_ISRTail:   .rel.text offset 0x00000f70 -> pcie_set_outbound
```

The raw ARM code around those calls copies a DMAC tail profile into the MMR
window, ORs a start/control bit into MMR `+0x08`, and then calls
`pcie_set_outbound(profile+0x38, profile+0x39, profile+0x3a)` when the
profile gate around `+0x3b` allows it. This strongly suggests the real host
DMA advertisement is produced by the firmware DMAC profile path, not by Windows
directly writing BAR5 outbound registers. Linux exposes this through:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/firmware_dmac_outbound_path
```

`libmassmemaccess.so.9` is the firmware userspace wrapper around
`/dev/vpl_dmac`. It opens that node from `MassMemAccess_OpenDMAC`, and its
`MassMemAccess_StartDMAC` routine builds the 0x3c-byte DMAC profile consumed by
`VPL_DMAC_StartTail`:

```text
MassMemAccess_StartDMAC:
  profile backing buffer: object+0x4c
  copies/fills profile bytes 0x08..0x38
  uses MemMgr_GetPhysAddr for source/destination buffers
  calls MemMgr_CacheCopyBack(memmgr, profile, 0x3c)
  loops on ioctl(fd, 0xde00) until it returns 0

MassMemAccess_WaitDMAC:
  loops on ioctl(fd, 0xde01)

vpl_dmac.ko Ioctl:
  0xde00 calls VPL_DMAC_StartHead, then VPL_DMAC_StartTail
  0xde01 waits for channel completion
  0x8004de03 reads/checks DMAC version
  0x4004de02 writes/sets MMR mapping information
```

This makes the next implementation target more precise: host-side Linux should
not keep guessing BAR5 writes. It needs to reproduce the event/config path that
causes firmware `video_capture_mgr` plus `libmassmemaccess` to build this DMAC
profile, or add a guarded diagnostic that mirrors the exact 0x3c profile layout
once the profile fields are fully mapped.

Additional `libmassmemaccess.so.9` decode:

```text
MassMemAccess_StartDMAC profile build:
  object+0x4c -> profile pointer
  profile+0x08 = control word assembled from object+0x20, +0x24, +0x14,
    and mode object+0x1c
  profile+0x10 = phys(object+0x58) or raw object+0x58 depending object+0x48
  profile+0x14 = object+0x5c or phys(object+0x5c)
  profile+0x18 = object+0x28
  profile+0x1c = object+0x30
  profile+0x20 = object+0x34
  profile+0x24 = object+0x38
  profile+0x28 = object+0x3c
  profile+0x2c = object+0x40
  profile+0x30 = phys(object+0x54), mode-dependent
  profile+0x34 = object+0x44
  profile+0x38..0x3b = bytes object+0x60..0x63

VPL_DMAC_StartTail copies profile+0x08 and +0x10..+0x34 directly into the DMAC
MMR window, optionally calls pcie_set_outbound(profile[0x38], profile[0x39],
profile[0x3a]) when profile[0x3b] is non-zero, then starts DMA with
MMR+0x08 = profile+0x08 | 6.
```

## Firmware VIC Start Path

`video_capture_mgr` does not directly run the DMAC. On a SET_VIC event it
selects a YUY2/YV12 sensor config and launches `tinyvenc7` or `tinyvenc5` with
the captured geometry/color arguments. Those binaries use `libvideocap.so.13`.

Decoded `libvideocap.so.13`:

```text
VideoCap_Start:
  calls VideoCap_StartVIC

VideoCap_StartVIC:
  reads object+0x74 to select a VIC register slot
  ORs control bits into the selected VIC MMR word:
    +0x500 when object+0x24c == 1
    +0xe8 always before start
  loops ioctl(fd, 0x0000e313) until success

VideoCap_GetBufVIC:
  ioctl(fd, 0x8078e303) fills a 0x78-byte buffer/frame record
```

Decoded `vpl_vic.ko`:

```text
ioctl 0x0000e313:
  waits/checks VIC state in the kernel-side VIC object; this is the actual
  firmware VIC start/wait transition used by tinyvenc after SET_VIC.

ioctl 0x8078e303:
  copies a 0x78-byte buffer/frame record to userspace for VideoCap_GetBufVIC.
```

Implication: host commands `0x29/0x2a` only enqueue the endpoint/userland event.
The real capture path is:

```text
host endpoint event 0x29 SET_VIC
  -> video_capture_mgr
  -> tinyvenc7/5
  -> libvideocap VideoCap_StartVIC ioctl 0xe313
  -> libvideocap VideoCap_GetBufVIC ioctl 0x8078e303
  -> MassMemAccess / VPL_DMAC profile path for memory movement
```

The Linux host still needs the reliable endpoint event transport/hready state
that makes `video_capture_mgr` launch tinyvenc. Direct host writes to VIC/DMAC
MMRs are still not justified.

## Windows Commands After 0x29/0x2a

`/home/wozt/mz0380-decompiled/MZ0380_StartFirmware.c` shows an additional
stream-start caveat. After command `0x29` sets bit `0x100` and command `0x2a`
sets bit `0x200`, Windows can continue with:

```text
cmd 0x2d, length 0x0c dwords, format/scaler/timing payload
cmd 0x31, length 0x07 dwords, final stream/timing payload
```

Linux currently sends only `0x29 + 0x2a + 0x02` in `stream_start_test` and in
the V4L2 real-DMA startup path. Do not add `0x2d/0x31` blindly: the payloads are
assembled from mode-table values in `MZ0380_StartFirmware.c`, not fixed obvious
constants. If `preinit_command1` works again after a full power cycle but
`stream_start_test` still produces no non-mailbox IRQs or buffer writes, the
next static target is to decode the exact 1080p60 `0x2d` and `0x31` payloads
and test them behind an explicit experimental module parameter.

Linux now exposes this decode as a read-only debugfs node:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_extra_commands
/sys/kernel/debug/hd60prodrv/0000:22:00.0/windows_stream_scale_table
```

That node documents the success-bit gates and packed packet shapes:

```text
0x29 success -> bit 0x0100
0x2a success -> bit 0x0200
primary 0x2d success -> bit 0x0400
secondary 0x2d success -> bit 0x0800
0x31 success -> bit 0x1000

primary 0x2d:
  [0x800, 0x2d, 0x3fff, w3..w11], length 0x0c dwords

secondary 0x2d:
  same shape, using the secondary/high-half table values

0x31:
  [0x800, 0x31, 0x3f, w3..w6], length 0x07 dwords
```

This is diagnostic only. V4L2 and `stream_start_test` still do not send these
commands.

The fixed data-table extraction is now reproducible:

```sh
./scripts/decode-mz0380-stream-tables.py
./scripts/decode-mz0380-stream-tables.py --packet-model
```

It reads:

```text
LXV4L2D_MZ0380.ko .data+0x1ed8 scale_tb, size 240
LXV4L2D_MZ0380.ko .data+0x15798 SC2CC_VIN_MAP, size 32
LXV4L2D_MZ0380.ko .data+0x1fc8 TABLE_DEVICE_INPUT_TOPOLOGY, size 4208
```

Recovered `scale_tb` has `scale_tb[4] = 1920 x 1080` (`0x0780 x 0x0438`),
which matches the target 1080p row. `SC2CC_VIN_MAP` is:

```text
0,2,1,3,4,6,5,7
```

Important negative result: these fixed symbols still do not provide complete
sendable `0x2d/0x31` packets. The disassembly shows `MZ0380_StartFirmware`
loads most `0x2d/0x31` words from runtime device/stream-state offsets and
sanitized stack temporaries around `sp+0x140..0x240`. The next useful step is
to recover or observe those runtime fields, not to hard-code the scale row as a
complete packet.

Exact ARM-context trace targets for those runtime fields:

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

These are word indexes from the decompiled ARM context, not PCI BAR offsets.

The packet model was cross-checked with a fresh Ghidra 12.1.2 headless export
from the existing `/home/wozt/ghidra-projects/MZ0380` project:

```sh
/home/wozt/logiciels/ghidra_12.1.2_PUBLIC/support/analyzeHeadless \
  /home/wozt/ghidra-projects MZ0380 \
  -process LXV4L2D_MZ0380.ko -noanalysis \
  -scriptPath scripts/ghidra \
  -postScript ExportMZ0380Stream.java /tmp/mz0380-ghidra-stream
```

Important correction from the fresh Ghidra export: packet dwords follow the
stack order `local_38, uStack_34, local_30, local_2c, low(local_28),
high(local_28), local_20, uStack_1c, local_18, uStack_14, local_10, uStack_c`.
That means the derived/reduced scaler word in `0x2d` is packet `w7`, not `w5`
or `w6`. Use `--packet-model` for the current mapping.

## Windows Ghidra Export 2026-08-22

The Windows SYS project was also exported through Ghidra headless with the
address-based helper:

```sh
/home/wozt/logiciels/ghidra_12.1.2_PUBLIC/support/analyzeHeadless \
  /home/wozt ElgatoHD60pro \
  -process e60MZ0380.X64.SYS -noanalysis \
  -scriptPath scripts/ghidra \
  -postScript ExportFunctionsByAddress.java /tmp/hd60pro-ghidra-windows \
    send_command=140285074 \
    preinit_140278bb0=140278bb0 \
    isr_140284380=140284380 \
    base_fw_download=140275f64 \
    full_fw_download=1402762d4 \
    fw_version_query=140277c78
```

Export summary:

```text
send_command        0x140285074 FOUND
preinit_140278bb0   0x140278bb0 FOUND
isr_140284380       0x140284380 FOUND
base_fw_download    0x140275f64 FOUND
full_fw_download    0x1402762d4 FOUND
fw_version_query    0x140277c78 FOUND
```

Two older notes used `0x140278f80` and `0x14028d8d1`; those did not resolve to
functions in the current Ghidra project and should be treated as stale until
re-found.

Confirmed Windows mailbox facts from fresh Ghidra:

```text
send_command 0x140285074:
  mmio base is device+0x108
  synchronous path clears base+0x2c, writes packet[1..n-1], writes base+0 = 0x800,
    then polls base+0x2c bit0 up to 50 iterations
  async path initializes semaphore/event at device+0x69d0, writes packet, rings
    base+0 = 0x800, then waits in 0x14028ce04

preinit 0x140278bb0:
  writes device+0x110+0xdc = 2
  writes device+0x108+0x30 = 0
  writes device+0x108+0x00 = 0x400
  loops up to 100 times:
    device+0x110+0x30 = device physical BAR0 + 4
    device+0x110+0x38 = device physical BAR0 + 0x5f
    async packet [0x800, 0x01], length 2

ISR/thread 0x140284380:
  reads device+0x108+0x30
  ACKs with device+0x110+0xdc = 2, device+0x108+0x30 = 0,
    device+0x108+0x00 = 0x400
  if status bit 11 is set, releases device+0x69d0
```

Firmware download sequences:

```text
base firmware 0x140275f64:
  [0x800, 0x0e, selector, file_size], async, timeout 0x02faf080
  copy file bytes to device+0x108 + 0x60
  [0x800, 0x0f, 1], async, timeout 0x6b49d200
  sleep, success if read32(device+0x108 + 0x08) == 0

full firmware 0x1402762d4:
  [0x800, 0x0b, file_size], async, timeout 0x02faf080
  copy file bytes to device+0x108 + 0x60
  [0x800, 0x0c, 1], async, timeout 0x23c34600
  sleep 100 ms, success if read32(device+0x108 + 0x08) == 0
```

This reinforces BAR0 as the firmware-copy aperture and BAR5 as the sideband/IRQ
register block. After a good cold-boot `preinit_command1`, the next Linux code
target is a guarded base/full firmware download diagnostic using these packet
sequences, not the removed post-logo replay path.
