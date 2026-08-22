# DMAC Profile Reverse Notes

Sources used for this note:

- `/tmp/vpl-dmac-decode.txt`
- `/tmp/mma-set-options.txt`
- `/tmp/mma-start-one-frame.txt`
- `/tmp/mma-process-one-frame.txt`
- `docs/claude-handoff-2026-08-22.md`

## Core Conclusion

The capture-frame path should not be implemented as direct Linux BAR5/BAR0
outbound-window pokes.

The ARM firmware path builds a 0x3c-byte `libmassmemaccess` profile in
userspace, flushes it, then submits it to `/dev/vpl_dmac`. `vpl_dmac.ko` copies
that profile into the DMAC MMR window and only then optionally calls
`pcie_set_outbound()`.

The three `pcie_set_outbound()` arguments are bytes from `profile+0x38..0x3a`.
They are endpoint outbound controls/channel selectors. They are not the host
frame physical address. The transfer addresses and geometry are the profile
words copied to DMAC MMR offsets `+0x10..+0x34`.

## `vpl_dmac.ko` Tail Submission

`VPL_DMAC_StartTail` at `0x0b5c`:

```text
next_head = (ring_index + 1) & 0x3f
ring[ring_index] = profile_pointer
object+0x140 = next_head

if busy:
  return
if head == tail:
  return

busy = 1
MMR_STATUS |= 0x400

if profile[0x3b] != 0:
  pcie_set_outbound(profile[0x38], profile[0x39], profile[0x3a])

MMR+0x08 = profile+0x08
MMR+0x10 = profile+0x10
...
MMR+0x34 = profile+0x34
MMR+0x08 = profile+0x08 | 6
```

`VPL_DMAC_ISRTail` mirrors this after completion: it advances tail
`(tail + 1) & 0x3f`, loads the next ring profile, optionally calls
`pcie_set_outbound()`, copies the same MMR words, and writes `profile+0x08 | 6`.
When no more profiles remain it clears MMR bit `0x400` and clears busy.

The ioctl decode is:

```text
0xde00      -> VPL_DMAC_StartHead, then VPL_DMAC_StartTail
0xde01      -> wait for channel completion
0x4004de02  -> MMR mapping/setup
0x8004de03  -> version/check path
```

## Profile Layout

The profile is 0x3c bytes. `MassMemAccess_StartDMAC` builds it at the pointer
held in `object+0x4c`, flushes exactly 0x3c bytes, and submits ioctl `0xde00`.

```text
profile+0x08 -> DMAC MMR+0x08, control; final start write uses value | 6
profile+0x10 -> DMAC MMR+0x10, address/phys word 0
profile+0x14 -> DMAC MMR+0x14, address/phys word 1
profile+0x18 -> DMAC MMR+0x18, transfer field
profile+0x1c -> DMAC MMR+0x1c, transfer field
profile+0x20 -> DMAC MMR+0x20, transfer field
profile+0x24 -> DMAC MMR+0x24, transfer field
profile+0x28 -> DMAC MMR+0x28, transfer field
profile+0x2c -> DMAC MMR+0x2c, transfer field
profile+0x30 -> DMAC MMR+0x30, optional phys/window address
profile+0x34 -> DMAC MMR+0x34, transfer field
profile+0x38 -> pcie_set_outbound arg0 byte, from object+0x60
profile+0x39 -> pcie_set_outbound arg1 byte, from object+0x61
profile+0x3a -> pcie_set_outbound arg2 byte, from object+0x62
profile+0x3b -> outbound enable gate byte, from object+0x63
```

`profile+0x08` is assembled from:

```text
object+0x20 << 4
object+0x24 << 6
object+0x14 << 10
mode bits from object+0x1c:
  0 -> no extra bits
  1 -> +0x100
  2 -> +0x200
  other -> +0x300
```

Known object-to-profile mapping from the existing `MassMemAccess_StartDMAC`
decode:

```text
object+0x4c -> profile pointer
object+0x48 -> raw/physical address selection
object+0x58 -> profile+0x10, converted with MemMgr_GetPhysAddr in some modes
object+0x5c -> profile+0x14, converted with MemMgr_GetPhysAddr in some modes
object+0x28 -> profile+0x18
object+0x30 -> profile+0x1c
object+0x34 -> profile+0x20
object+0x38 -> profile+0x24
object+0x3c -> profile+0x28
object+0x40 -> profile+0x2c
object+0x54 -> profile+0x30, only in some modes after phys conversion
object+0x44 -> profile+0x34
object+0x60..0x63 -> profile+0x38..0x3b
```

## `MassMemAccess_SetOptions`

`MassMemAccess_SetOptions` at `0x1094` recognizes at least these option IDs:

```text
0x49:
  object+0x10 = opt+0x04

0x4e:
  object+0x14 = opt+0x04

0x50:
  object+0x60 = (u8)opt+0x04
  object+0x61 = (u8)opt+0x08
  object+0x62 = (u8)opt+0x0c
  object+0x63 = 1

0x17:
  opt+0x04 = base address
  opt+0x08 = total byte count
  opt+0x0c = stride/count field
  opt+0x10 = raw/phys flag
```

Option `0x50` is the exact source of the `profile+0x38..0x3b` outbound bytes.

Option `0x17` rejects unaligned byte counts (`size & 7`) and requires a nonzero
base, size, stride, and `object+0x50`. If size is greater than `0x800`, it splits
work into chunks:

```text
chunks = (size + 0x7ff) >> 11
per_chunk_words = size >> 3
source_base advances by 0x100 per chunk
window_base advances by stride << 8 per chunk
last chunk is rounded/aligned from remaining words
```

For size up to `0x800`, it emits one helper request:

```text
object/request +0x34 = size >> 3
object/request +0x38 = size >> 3
object/request +0x40 = ((size + 0x3f) >> 6) << 3
```

The helper at `0xaa0` is the per-request setup path; helper `0xa64` starts the
DMAC profile, and helper `0x9d4` is the wait path.

## `MassMemAccess_StartOneFrame`

`MassMemAccess_StartOneFrame` at `0x19f8` maps a request object into the
MassMemAccess object, then calls helper `0xa64`.

Common setup:

```text
request+0x08 -> object+0x48
MemMgr_GetPhysAddr(request+0x34) -> local r7, unless request+0x08 == 1
MemMgr_GetPhysAddr(request+0x38) -> local r6, unless request+0x08 == 1
```

For `request+0x30 == 0`, normal mode (`request+0x04 != 1` and
`request+0x00 != 1`):

```text
request+0x0c -> object+0x20
request+0x10 -> object+0x24
request+0x14 -> object+0x28
request+0x30 -> object+0x1c
request+0x34 -> object+0x58
request+0x38 -> object+0x5c
request+0x2c -> object+0x44
call helper 0xa64
```

For `request+0x04 == 1`:

```text
request+0x10 -> object+0x24
request+0x38 -> object+0x5c
request+0x30 -> object+0x28
request+0x1c -> object+0x34
request+0x20 -> object+0x38
request+0x24 -> object+0x3c
request+0x28 -> object+0x40
object+0x1c = 2
```

For `request+0x00 == 1`:

```text
request+0x0c -> object+0x20
request+0x10 -> object+0x24
request+0x30 -> object+0x28
request+0x34 -> object+0x58
request+0x38 -> object+0x5c
request+0x18 -> object+0x30
request+0x1c -> object+0x34
request+0x20 -> object+0x38
request+0x24 -> object+0x3c
object+0x1c = 1
```

For `request+0x30 != 0`, small mode only (`request+0x1c <= 0x800`) and
`object+0x50 != 0`:

```text
request+0x0c -> object+0x20
request+0x10 -> object+0x24
request+0x34 -> object+0x58
request+0x38 -> object+0x5c
request+0x18 -> object+0x30
request+0x1c -> object+0x34
request+0x20 -> object+0x38
request+0x24 -> object+0x3c
request+0x28 -> object+0x40
object+0x50 -> object+0x54
object+0x1c = 3
object+0x28 = 0
call helper 0xa64
```

If that mode is larger than `0x800`, `StartOneFrame` returns an error; the
chunked path is `MassMemAccess_ProcessOneFrame`.

## `MassMemAccess_ProcessOneFrame`

`MassMemAccess_ProcessOneFrame` at `0x1698` is similar to `StartOneFrame` for
simple modes, but it handles `request+0x30 != 0` with chunking.

For the chunked mode:

```text
chunks = (request+0x1c + 0x7ff) >> 11
object+0x20 = request+0x0c
object+0x24 = request+0x10
object+0x40 = request+0x28
object+0x1c = 3
object+0x28 = 0

for chunk_index in 0..chunks-1:
  offset = chunk_index << 11
  object+0x58 = request+0x34 + offset
  object+0x5c = request+0x38 + offset
  object+0x30 = request+0x18
  object+0x34 = min(0x800, remaining bytes)
  object+0x38 = request+0x20
  object+0x54 = object+0x50 + chunk_index * (request+0x20 << 8)
  object+0x3c = request+0x24
  call helper 0xa64
  call helper 0x9d4
```

This is the strongest current evidence that frame movement is firmware-owned
and chunked through the MassMemAccess object/profile machinery.

## Linux Implementation Implication

Current Linux `cmd 0x02` sends four 32-bit coherent host frame addresses to the
firmware mailbox. That may be necessary, but there is no decoded evidence yet
that `cmd 0x02` alone causes firmware to build the MassMemAccess request fields
above or submit `/dev/vpl_dmac` ioctl `0xde00`.

The next useful reverse target is tinyvenc's call site for
`TK_MMA_StartOneFrame`/`TK_MMA_ProcessOneFrame`.

## VIC/Tinyvenc Link

`readelf -Ws` shows `tinyvenc5`, `tinyvenc7`, and `tinyvenc8` all import:

```text
VideoCap_GetBufVIC
VideoCap_StartVIC
TK_MMA_StartOneFrame
TK_MMA_ProcessOneFrame
TK_MMA_WaitOneFrameComplete
TK_MMA_SetOptions
```

This means the likely video-frame path is:

```text
video_capture_mgr SET_VIC
  -> launches tinyvenc7 or tinyvenc5
  -> tinyvenc calls VideoCap_StartVIC / VideoCap_GetBufVIC
  -> tinyvenc passes selected VIC buffer fields to TK_MMA_*
  -> libtk_mass_mem_access.so.0 forwards to MassMemAccess_*
  -> /dev/vpl_dmac ioctl 0xde00 / 0xde01
```

`TK_MMA_StartOneFrame` and `TK_MMA_ProcessOneFrame` are thin wrappers:

```text
if r0 == NULL:
  return -1

object = r0
massmem_object = *(u32 *)object
request = object + 4
object+0x3c = caller r1
object+0x38 = caller r2
object+0x18 = caller r3
call MassMemAccess_StartOneFrame or MassMemAccess_ProcessOneFrame
```

So tinyvenc controls at least three wrapper-level request fields directly via
the `r1/r2/r3` arguments, while the rest of the request object starts at
`mma_wrapper + 4`.

`VideoCap_GetBufVIC` issues `ioctl(fd, 0x8078e303)` into a stack record and
copies it into the caller's 0x80-ish-byte output record. Important observed
translation:

```text
stack+0x5c -> MemMgr_GetVirtAddr -> out+0x38
stack+0x60 -> MemMgr_GetVirtAddr -> out+0x3c
stack+0x64 -> MemMgr_GetVirtAddr -> out+0x40
stack+0x90 low 13 bits -> out+0x54
stack+0x90 bits 16..28 -> out+0x50
ioctl source fields around stack+0x78..0xa0 -> out+0x58..0x7c
```

`VideoCap_StartVIC` sets VIC control bits and loops on `ioctl(fd, 0xe313)`
until it succeeds.

Concrete next reverse task: disassemble tinyvenc7 around the calls to the PLT
entries for `VideoCap_GetBufVIC`, `TK_MMA_StartOneFrame`, and
`TK_MMA_ProcessOneFrame`, then map the tinyvenc frame record to the
MassMemAccess request offsets listed above.

Do not add guessed DMAC MMR writes in Linux. Either:

1. Trigger the firmware path that builds this profile, or
2. Mirror a fully decoded profile/MMR submission after the request construction
   is understood.
