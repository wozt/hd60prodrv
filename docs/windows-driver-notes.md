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

Local Linux test result after reproducing the `0x01` pre-init:

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

The first isolated Linux test for this region is limited to:

```text
packet = [0x800, 0x17, 0x00000001, 0x00000001]
```

This corresponds to `0x140287c80(line=0, value=1)`. The debugfs endpoint
`gpio17_line0` refuses to run unless the preceding `0x0a` state is validated as
`BAR0+0x2c == 0xaaaaaaaa` and firmware version `01.11`.

Local result:

```text
packet: [0x800, 0x17, 0x00000001, 0x00000001]
result: 0
completion: 0x00000001
BAR0+0x08: 0x00000001
BAR0+0x0c: 0x00000001
```

The card stayed accessible. The next Linux test is `gpio17_generic_sequence`,
which sends the eight decoded generic-branch `0x17` packets and stops at the
first failure.

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

The Linux diagnostic endpoint `i2c1a_c0_dc_db` performs only these two reads
with command `0x1a` after replaying the validated pre-init/status/GPIO path.

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
helper script `scripts/extract-logo-payloads.py` extracts the observed raw
blocks from `.data` in `e60MZ0380.X64.SYS`, and
`scripts/install-logo-payloads-root.sh` installs the three selector payloads to
`/lib/firmware/hd60prodrv/`.

Linux currently exposes `logo_upload_plan` as a read-only debugfs file. It
checks payload availability and prints the planned Windows packets, but it does
not write the hardware. `logo_upload_selector100` uploads only the first logo
for isolation, while `logo_upload_all` reproduces the observed Windows sequence
for selectors `0x100`, `0x200`, and `0x300`.

## Post-Logo Command 0x1d Writes

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

Linux exposes this isolated reconstruction as
`post_logo_pipeline_28548c_min`, gated behind
`allow_post_logo_pipeline=1`. It must be run immediately after
`logo_upload_all`, before the `0x1d` writes.

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
uploads and are now replayed explicitly by `pre_28548c_logo_uploads_local`.

## Current Linux Bring-Up State

`scripts/test-post-logo-cmd1d-root.sh` now runs the reconstructed sequence
through `post_logo_cmd1d_a2`, then runs `post_logo_challenge_a2` and requires
`pipeline_ready: 1`. This gives the loaded driver a persistent in-memory
pipeline-ready state for the next capture/DMA work.

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
