# HD60 Pro Audio Path Notes

Status: audio is not implemented as a host ALSA device yet. The firmware-side
audio path is now partially decoded and should guide the Linux implementation.

## Firmware Userland Flow

Useful firmware root:

```text
/tmp/claude-1000/-home-wozt-hd60prodrv/d9769f12-0974-4a61-8572-41b3cab7d6cb/scratchpad/fw_extract/yuan_demo_sdi
```

Relevant binaries:

```text
audio_capture_mgr
capture_audio_8ch
drivers/gv7601_audio.ko
```

`audio_capture_mgr` opens `/sys/vpl_pciep/audio_ctrl`, reads/polls 0x2c-byte
records, and acknowledges handled records with `pwrite(..., 0x2c, offset 0)`.
Command `0x2a` is the Set AIC params/start-audio path. Its firmware printf
names these fields:

```text
bits, channel_num, mono, freq, frame_num_of_period,
period_num_of_buffer, on
```

For HD, it launches:

```text
./capture_audio_8ch -D -P <periods> -d <i2s> -R <freq> -F <frames_per_period> -B <bits>
```

It can force the 8-channel path (`-d 8`) and toggles endpoint readiness with:

```text
echo '0' > /sys/vpl_pciep/hready
echo '1' > /sys/vpl_pciep/hready
```

## capture_audio_8ch

The stripped main function is at `0x8eb8`; the PCM/MMA loop is at `0x9948`.
Ghidra exports are in:

```text
/tmp/mz0380-ghidra-audio-1787359216/
```

The app opens four ALSA capture devices:

```text
hw:0,0
hw:0,1
hw:0,2
hw:0,3
```

Each device is configured as stereo PCM:

```text
access: RW_INTERLEAVED
format: S16_LE
channels: 2
rate: from -R, typically 48000 for HD
period size: from -F, firmware string recommends 256 for HD
buffer periods: from -P, firmware string recommends 4 for HD
```

The loop reads each active stereo PCM with `snd_pcm_readi()`, recovers
overruns with `snd_pcm_prepare()`, then sends the packed period through the
firmware mass-memory-access path:

```text
MemBroker_GetMemory(period_bytes << 6, 1)
TK_MMA_Init(0, 2, 0x20, 1)
TK_MMA_SetOptions(...)
MemBroker_GetPhysAddr(pcm_buffer)
TK_MMA_ProcessOneFrame(handle, endpoint_addr, phys_pcm_buffer, period_bytes << 2)
pwrite(channel_done_fd, record, 0x18, 0)
```

The decoded endpoint address expression is:

```text
((channel_state + 0x3ffffff) * 0x4000) - 0x70000000
```

## Linux Implication

Do not add a fake/silent ALSA device as proof of audio. The real target is:

1. Make the firmware event path reliable enough for command `0x2a`.
2. Map the 0x18-byte `/sys/class/vpl_pciep/channel_done` record.
3. Connect the host-side raw-audio tasklet/ring to an ALSA capture PCM.

The current driver exposes the static decode at:

```text
/sys/kernel/debug/hd60prodrv/0000:22:00.0/firmware_audio_path
```
