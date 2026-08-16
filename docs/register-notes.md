# Register Notes

These notes are observations, not a confirmed register map.

## Snapshot 2026-08-15 Initial Loaded Probe

Device:

```text
PCI:       12ab:0380
Subsystem: 1cfa:0006
BAR0:      32 MiB
BAR5:      4 KiB
```

PCI command after probe:

```text
0x0002
```

Only memory space is enabled. Bus mastering is still disabled, which is
intentional until the DMA register layout is known.

Capabilities:

```text
PM:        0x40
MSI:       0x50
PCIe:      0x70
MSI-X:     absent
```

## BAR0

First 256 bytes:

```text
0000: ffffffff 0000000a 00000001 0000000b
0010: 00000000 00000000 00000000 00000000
...
0060: ffffffff ffffffff ffffffff ffffffff
...
```

Interpretation:

- BAR0 is the likely Windows `device+0x108` mapping.
- Current disassembly evidence points to BAR0 as the mailbox and firmware-copy
  aperture used by `MZ0380_SEND_COMMAND` and `MZ0380_DownloadFirmware`.
- It may also be a large aperture, SRAM/DDR/FPGA memory window, or sparse
  internal address space.
- Reads returning `0xffffffff` across ranges suggest unmapped/invalid regions.
- The words at `0x04`, `0x08`, and `0x0c` are the only immediately interesting
  values in the initial head dump.

Do not read broad BAR0 ranges automatically. On this card, reading `bar0_4k`
caused the debugfs reader to stall after the initial 256-byte head read. Keep
BAR0 probing to small, known-safe offsets until the aperture behavior is better
understood.

Next data needed:

- Avoid BAR0 in automatic snapshots. If BAR0 must be probed again, do it with a
  one-off timeout and expect some offsets to stall.

## BAR5

First 256 bytes:

```text
0000: 11000001 00010001 03b02004 00000000
0010: 00000000 00000000 c7000030 00000000
0030: fa000004 00000000 fa00005f 00000000
0050: 00400208 00000000 00000000 00000000
00d0: 00000000 00f20000 00000000 00000000
00e0: 11000001 11000001 11000001 11000001
00f0: 11000001 11000001 11000001 11000001
```

Interpretation:

- BAR5 is likely a secondary/sideband MMIO control block.
- `0x0000` and the repeated `0x11000001` values may be block identity/status
  defaults or mirrored read behavior.
- `0x0030 = 0xfa000004` and `0x0038 = 0xfa00005f` match the Windows
  `HwInitialize` writes of BAR0 physical low32 plus `0x4` and `0x5f`.
- `0x0050 = 0x00400208` and `0x00d4 = 0x00f20000` are candidate status/config
  registers.
- A full BAR5 dump shows the first `0x200` bytes mirrored through the 4 KiB BAR,
  so the useful BAR5 window is probably 512 bytes.
- Windows driver disassembly shows a command helper that writes DWORD arguments
  to `device+0x108` at offsets `0x04`, `0x08`, ... and then writes
  `0x00000800` to offset `0x00` as a likely doorbell. The Windows resource
  mapping loop currently indicates that `device+0x108` is BAR0 on this card.
  BAR5 still appears in the DPC/interrupt path as `device+0x110`.

Observed Windows helper shape for the polling path:

```text
bar0[0x2c] = 0
bar0[0x04] = packet[1]
bar0[0x08] = packet[2]
...
bar0[0x00] = 0x00000800
wait event device+0x69d0
```

Observed Windows pre-init shape before visible firmware download:

```text
bar5[0xdc] = 2
bar0[0x30] = 0
bar0[0x00] = 0x400
bar5[0x30] = low32(bar0_phys + 0x4)
bar5[0x38] = low32(bar0_phys + 0x5f)
send packet [0x800, 0x01] through the async path
```

After this command, the local card stayed accessible and reported:

```text
bar0[0x2c] = 0xdddddddd
bar0[0x08] = 0x00000001
bar0[0x0c] = 0x0000000b
```

The `0xdddddddd` value is not treated as a failure for this path because
Windows uses the async event/IRQ path. The `0x08/0x0c` values match
`MZ0380.FW.TXT` version `01.11`.

The next Windows command in `HwInitialize` is:

```text
send packet [0x800, 0x0a, 0, 0] through the async path
look for bar0[0x2c] == 0xaaaaaaaa
then compare bar0[0x08] / bar0[0x0c] against MZ0380.FW.TXT
```

Local test result: immediate reads after command `0x0a` can still show
`0xdddddddd` and zero result words, but the later health dump showed:

```text
bar0[0x2c] = 0xaaaaaaaa
bar0[0x08] = 0x00000001
bar0[0x0c] = 0x0000000b
```

So the Linux diagnostic must wait for the `0xaaaaaaaa` marker before deciding
whether the firmware version matches.

Observed I2C-like write wrapper builds this packet:

```text
packet[0] = 0x00000800
packet[1] = 0x0000001b
packet[2] = i2c address, e.g. 0x50
packet[3] = register, e.g. 0x61
packet[4] = value
```

Linux now executes only guarded, reconstructed `0x1b` subsets:

- `post_logo_pipeline_28548c_min`: `9c:00=0x02`, then `9c:18=0x00`.
- `post_logo_pipeline_24dc28_head_local`: beginning of `0x14024dc28`,
  GPIO-alt line 9 high/low/high with 50 ms sleeps, GPIO-alt line 8 low, then
  `9c:00=0` and `9c:13=8`.
- `post_logo_pipeline_24dc28_table1_local`: first fixed continuation of
  `0x14024dc28`, through local/default writes ending at bank1 `9c:19=2`.
- `post_logo_pipeline_24dc28_table2_local`: next fixed continuation of
  `0x14024dc28`, writing bank1 `9c:1a=0x50`, reading bank1 `9c:2a`,
  writing `9c:2a=(read|0x07)`, then selecting bank2 and writing `9c:08=3`.
- `post_logo_pipeline_24dc28_table3_local`: continuation of `0x14024dc28`
  through the first `9c:24` branch and the following local/default bank1/bank0
  writes, ending at bank0 `9c:b4=(read&0xfc)`. It preserves the Windows
  conditional `9c:24` bit0 branch instead of forcing the optional `25/26/27`
  writes.
- `post_logo_pipeline_24dc28_table4_local`: bank2 continuation of
  `0x14024dc28`, starting after `table3` and ending at `9c:2e=(read|0xa1)`.
  It stops before the repeated bank0 `ab/ac` RMW and before the next nested
  helper calls.
- `post_logo_pipeline_24dc28_table5_local`: local/default continuation after
  `table4`, repeating the bank0 `ab/ac` RMW sequence, then the decoded helpers
  `0x14024eeb8`, `0x14024d2a4`, and `0x14024db30`, followed by bank0
  `9c:51=0x89`, a second `0x14024eeb8` call with `edx=0x30`, and
  `9c:b7=0`.
- `post_logo_pipeline_24dc28_table6_local`: beginning of helper
  `0x14024d2ec`, applying the fixed bank2 writes and bank0 RMW sequence through
  final `9c:00=(read&0x7f)`.
- `post_logo_pipeline_24dc28_table7_local_default`: local/default tail of
  `0x14024d2ec` for assumed `context+0x9810=0` and `context+0x81dc=0`. It
  reads bank0 `9c:d0/cf` and rewrites them using Windows table byte `0x06`
  from `{06,00,04,03,07,01}`.
- `post_logo_pipeline_28548c_after_24dc28_tail`: local/default continuation
  after the `0x14024dc28` call in `0x14028548c`, writing `9a:31=1` and
  `88:03=0xa7`. Local hardware returned status `0x4d` for address `0x9a` and
  `0x44` for address `0x88`.
- `post_logo_pipeline_286734_local_noop`: documents the observed local path of
  `0x140286734`; with local bytes `0x0e=0` and `0x0f=0`, no mailbox write is
  emitted.
- `post_logo_pipeline_28548c_local_prefix`: true `0x14028548c` local call-site
  prefix currently isolated to decoded helper `0x1402851cc`, writing
  `88:03=0xa7` before `0x140287224`.
- `post_logo_pipeline_287224_min`: local/default prefix of `0x14024c894`,
  including banked reads `9c:4a/ab/ac`, writes
  `9c:ab=(read&0x95)|0x15`, `9c:ac=(read&0xd5)|0x15`, and tail writes
  `9c:ad=0`, `9c:1e=0x11`, `9c:1f=1`.
- `post_logo_pipeline_24c894_coeffs`: local/default coefficient writes from
  `0x14024cbf1..0x14024cdf2`, with helper `0x140274aec` writing each 16-bit
  value as `reg+1=(value>>8)&0x7f` followed by `reg=value&0xff`.
- `post_logo_pipeline_28548c_tail_local`: currently decoded local/default tail
  candidate after coefficients: select bank 2, write `9c:27=0`, then set
  GPIO-alt command `0x15` lines 10 and 11 high. On hardware, the final line
  11 command completed with `BAR0+0x08=0x800` and `BAR0+0x0c=0x1000`.

Do not execute additional `0x1b` table writes from Linux unless the matching
Windows helper has been decoded and reduced to the local/default path.

Observed extended write wrapper `0x140287b54` builds command `0x1d`:

```text
packet[0] = 0x00000800
packet[1] = 0x0000001d
packet[2] = device/address byte
packet[3] = register byte
packet[4] = value byte
packet[5] = 0x00000001
```

The three explicit post-logo calls target address `0xa2`, registers `0x11`,
`0x12`, and `0x10`. Linux exposes these only behind `allow_cmd1d_write=1`.
After the fuller local sequence through coefficients and GPIO tail, these
writes completed and left the mailbox at `0x1d a2:10=0x5a`.

Next data needed:

- Full 4 KiB BAR5 dump.
- Diff BAR5 across HDMI states.
- Diff BAR5 before and after V4L2 node creation only, if needed.

## Current Safety Boundary

Do not issue visible firmware command `0x0b` on cold hardware. The next risky
step, if tested, is only the Windows pre-init `0x01` path with IRQ enabled and
the unsafe visible firmware path still blocked.
