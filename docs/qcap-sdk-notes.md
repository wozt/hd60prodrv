# QCAP Linux SDK Notes

Local SDK archive:

```text
/home/wozt/Téléchargements/DVP Linux SDK v1.57.0(MZ0380).zip
```

The archive contains `lib/libqcap.x64.so`. It is an x86-64 userspace shared
library with debug info and exported C++ symbols, not a kernel driver. It is
still useful because it shows the official Linux userspace ABI expected above a
capture driver.

Re-run the inventory with:

```sh
./scripts/analyze-qcap-sdk.sh
```

Important current findings:

- The SDK uses a `V4L2_GENERAL2` backend and public `QCAP_*` APIs. This supports
  the current Linux-driver direction: expose a normal `/dev/video*` V4L2 capture
  node rather than a bespoke userspace ABI.
- `V4L2_GENERAL2::PrepareBuffers` uses standard mmap streaming:
  `VIDIOC_REQBUFS`, `VIDIOC_QUERYBUF`, `mmap`, and `VIDIOC_QBUF`.
- The format path uses standard `VIDIOC_G_FMT`, `VIDIOC_S_FMT`, and
  `VIDIOC_S_PARM` calls. The driver now implements `VIDIOC_G_PARM/S_PARM` and
  clamps the only exposed mode to 1080p60.
- The driver now exposes standard no-op V4L2 controls for brightness, contrast,
  saturation, hue, audio volume, and audio mute. They satisfy ordinary
  `VIDIOC_QUERYCTRL/G_CTRL/S_CTRL` probes from VLC/QCAP-style userspace while
  hardware-side tuning remains unknown.
- The SDK also sends private control IDs around `0x0099....` through
  `VIDIOC_S_CTRL`/`VIDIOC_S_EXT_CTRLS` for encoder/device-specific settings.
  These are userspace-library settings, not proof of a missing kernel mailbox
  command. Do not implement guessed private controls unless a real SDK sample
  requires them and the expected behavior is known.

Useful exported symbols include:

```text
QCAP_CREATE
QCAP_RUN
QCAP_STOP
QCAP_SET_VIDEO_DEFAULT_OUTPUT_FORMAT
QCAP_REGISTER_VIDEO_PREVIEW_CALLBACK
QCAP_REGISTER_AUDIO_PREVIEW_CALLBACK
V4L2_GENERAL2::Create
V4L2_GENERAL2::RunEx
V4L2_GENERAL2::SetVideoFormat
V4L2_GENERAL2::StartAI
V4L2_GENERAL2::StartVENC
V4L2_GENERAL2::PrepareBuffers
```

This SDK does not replace the Windows/ARM reverse engineering for mailbox,
firmware, or host DMA setup. It only validates and sharpens the Linux userspace
surface that VLC/QCAP will consume once real frames flow.
