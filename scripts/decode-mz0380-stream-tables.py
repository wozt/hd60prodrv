#!/usr/bin/env python3
import argparse
import struct


DATA_FILE_OFFSET = 0x74670
SYMBOLS = {
    "scale_tb": (0x1ED8, 240),
    "TABLE_DEVICE_INPUT_TOPOLOGY": (0x1FC8, 4208),
    "SC2CC_VIN_MAP": (0x15798, 32),
}

RUNTIME_TRACE_TARGETS = (
    (
        "param_1+0x1818 stream_state",
        "MZ0380_StartFirmware uses *(param_1+0x606) as unaff_x21; "
        "success_bits is word +0x639.",
        (
            0x013, 0x020, 0x025, 0x02A, 0x02F, 0x034, 0x039, 0x044,
            0x62D, 0x639,
        ),
    ),
    (
        "param_1+0x1944 window_table_a",
        "MZ0380_StartFirmware initializes puVar37 = param_1+0x651; "
        "primary/secondary 0x2d use low/high halves from these words.",
        (
            0x000, 0x008, 0x020, 0x028, 0x040, 0x048,
            0x060, 0x068, 0x080, 0x088,
        ),
    ),
    (
        "param_1+0x14878 window_table_b",
        "MZ0380_StartFirmware initializes puVar39 = param_1+0x521e; "
        "0x29/0x2a/0x2d/0x31 copy sanitized timing/window fields from here.",
        (
            0x000, 0x008, 0x040, 0x160, 0x1A0, 0x1A8,
            0x1C0, 0x1C8, 0x1E0, 0x1E8, 0x200, 0x208,
            0x220, 0x228, 0x240, 0x248, 0x260, 0x268,
            0x280, 0x288, 0x2A0, 0x2A8, 0x2C0, 0x2C8,
            0x320, 0x328, 0x330, 0x338,
        ),
    ),
    (
        "param_1+0x14000 board_state",
        "MZ0380_StartFirmware initializes puVar44 = param_1+0x5000; "
        "gates command family and 0x31 board/preset fields.",
        (
            0x08B, 0x08C, 0x08D, 0x08E, 0x08F, 0x597,
            0x598, 0x5B1, 0x5C4, 0x5C5, 0x5CF,
        ),
    ),
)

PACKET_MODELS = (
    (
        "cmd_0x29_set_vic",
        "length 0x0b dwords; success sets stream_state[0x639] bit 0x100",
        (
            ("w0", "0x00000800", "mailbox doorbell"),
            ("w1", "0x00000029", "SET_VIC_PARAMS"),
            ("w2", "stream_state[0x20] | min(stream_state[0x2a],0xff)<<8 | stream_state[0x13]<<16 | bVar34<<24", "channel/fps/color-ish gate"),
            ("w3", "window_table_b[0x330] | stream_state[0x25]<<16", "input/output height pair"),
            ("w4", "uStack_28 low dword", "mode/window low word from sanitized state"),
            ("w5", "uStack_28 high dword", "timing/mode high word"),
            ("w6", "window_table_b[0x1e0] | window_table_b[0x2c0]<<16", "table timing/crop pair"),
            ("w7", "window_table_b[0x000] | window_table_b[0x2a0]<<16", "format/timing pair"),
            ("w8", "bitstream_count | active_flag<<8 | param_1[0x3bd2]<<16 | table2<<24", "stream-count flags"),
            ("w9", "post_mode_or_aux << 24", "aux/post mode from earlier board selection"),
            ("w10", "chroma_enable | cr_offset<<8 | y_ref<<16 | cb_offset<<24", "defaults to 0x6ef02901 when board override is zero"),
        ),
    ),
    (
        "cmd_0x2a_post_set_vic",
        "length 0x06 dwords; gated by 0x29 success; success sets bit 0x200",
        (
            ("w0", "0x00000800", "mailbox doorbell"),
            ("w1", "0x0000002a", "POST_SET_VIC"),
            ("w2", "channel | post_mode<<8 | 0x00100000", "post_mode is normally 2,4,8 depending board family"),
            ("w3", "stream_state[0x62d]", "runtime stream/state token"),
            ("w4", "0x00000100 | post_aux<<16", "notify/audio/epint low dword"),
            ("w5", "board_flag | post_flag<<8", "notify/audio/epint high dword"),
        ),
    ),
    (
        "cmd_0x2d_primary",
        "length 0x0c dwords; gated by bits 0x300; success sets bit 0x400",
        (
            ("w0", "0x00000800", "mailbox doorbell"),
            ("w1", "0x0000002d", "format/scaler/timing"),
            ("w2", "0x00003fff", "mask"),
            ("w3", "channel | stream_state[0x2a]<<16 | window_table_a[0x20]<<24", "channel/fps/selector"),
            ("w4", "window_table_a[0x80] | window_table_b[0x000]<<8 | window_table_a[0x40]<<16 | 0x01000000", "source/color/offset"),
            ("w5", "window_table_a[0x60]", "high dword of uStack_28"),
            ("w6", "window_table_b[0x2c0]+1 | window_table_b[0x2a0]<<8 | window_table_b[0x1a0]<<16", "row/timing/misc"),
            ("w7", "reduced_h0 | reduced_w0<<16", "derived from scale factors and width/height"),
            ("w8", "low(window_table_a[0x000]) | low(window_table_a[0x008]>>12)<<16", "crop/window dimensions"),
            ("w9", "window_table_b[0x240] | window_table_b[0x220]<<16", "table pair 8/6"),
            ("w10", "window_table_b[0x280] | window_table_b[0x260]<<16", "table pair 12/10"),
            ("w11", "window_table_b[0x1c0] | window_table_b[0x1e0]<<16", "table pair 2/4"),
        ),
    ),
    (
        "cmd_0x2d_secondary",
        "length 0x0c dwords; gated by bits 0x700; success sets bit 0x800",
        (
            ("w0", "0x00000800", "mailbox doorbell"),
            ("w1", "0x0000002d", "format/scaler/timing"),
            ("w2", "0x00003fff", "mask"),
            ("w3", "channel | 0x100 | stream_state[0x2a]<<16 | (window_table_a[0x28] & 0x77ffffff)<<24", "secondary channel/fps/selector"),
            ("w4", "window_table_a[0x88] | window_table_b[0x008]<<8 | window_table_a[0x48]<<16 | 0x01000000", "secondary source/color/offset"),
            ("w5", "window_table_a[0x68]", "high dword of uStack_28"),
            ("w6", "window_table_b[0x2c8]+1 | window_table_b[0x2a8]<<8 | window_table_b[0x1a8]<<16", "secondary row/timing/misc"),
            ("w7", "reduced_h1 | reduced_w1<<16", "derived from secondary scale factors"),
            ("w8", "high(window_table_a[0x000]) | high(window_table_a[0x008]>>12)<<16", "secondary crop/window dimensions"),
            ("w9", "window_table_b[0x248] | window_table_b[0x228]<<16", "table pair 9/7"),
            ("w10", "window_table_b[0x288] | window_table_b[0x268]<<16", "table pair 13/11"),
            ("w11", "window_table_b[0x1c8] | window_table_b[0x1e8]<<16", "table pair 3/5"),
        ),
    ),
    (
        "cmd_0x31_final_timing",
        "length 0x07 dwords; gated by bits 0x0f00; success sets bit 0x1000",
        (
            ("w0", "0x00000800", "mailbox doorbell"),
            ("w1", "0x00000031", "final timing/control"),
            ("w2", "0x0000003f", "mask"),
            ("w3", "channel | stream_state[0x2a]<<8 | (window_table_b[0x320]+1)<<16 | window_table_b[0x330]<<24", "channel/fps/timing"),
            ("w4", "window_table_b[0x160] | board_state[0x598]<<16 | bVar11<<24", "mode/preset/flag"),
            ("w5", "(param_1[0x508e] == 0x50000005f) << 16", "board-family boolean"),
            ("w6", "(window_table_b[0x040] & 0xfff) | ((window_table_b[0x040] >> 12) & 0xfff)<<16", "dimension/timing split"),
        ),
    ),
)


def read_symbol(path, name):
    addr, size = SYMBOLS[name]
    with open(path, "rb") as f:
        f.seek(DATA_FILE_OFFSET + addr)
        return f.read(size)


def u32s(data):
    return struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])


def print_packet_models():
    print("\n== Stream packet models from MZ0380_StartFirmware ==")
    print("These are formulas, not safe sendable packets. Runtime fields must be traced.")
    for name, gate, words in PACKET_MODELS:
        print(f"\n{name}: {gate}")
        for word, expr, note in words:
            print(f"  {word:>3} = {expr}")
            print(f"        {note}")


def main():
    parser = argparse.ArgumentParser(
        description="Decode fixed stream tables from LXV4L2D_MZ0380.ko"
    )
    parser.add_argument(
        "ko",
        nargs="?",
        default="/home/wozt/mz0380-rootfs/usr/lib/modules/5.4.18-35-generic/misc/LXV4L2D_MZ0380.ko",
    )
    parser.add_argument(
        "--packet-model",
        action="store_true",
        help="Print only the stream command packet formulas and trace targets",
    )
    args = parser.parse_args()

    if args.packet_model:
        print_packet_models()
        print_runtime_trace_targets()
        return

    vals = u32s(read_symbol(args.ko, "scale_tb"))
    print("== scale_tb ==")
    for idx in range(0, len(vals), 2):
        row = idx // 2
        width, height = vals[idx], vals[idx + 1]
        suffix = "  target_1920x1080" if (width, height) == (1920, 1080) else ""
        print(f"{row:02d}: {width:4d} x {height:<4d}  0x{width:04x} x 0x{height:04x}{suffix}")

    vals = u32s(read_symbol(args.ko, "SC2CC_VIN_MAP"))
    print("\n== SC2CC_VIN_MAP ==")
    print(" ".join(str(v) for v in vals))

    vals = u32s(read_symbol(args.ko, "TABLE_DEVICE_INPUT_TOPOLOGY"))
    print("\n== TABLE_DEVICE_INPUT_TOPOLOGY matches for 12ab:0380 family ==")
    for idx in range(0, len(vals), 4):
        row = vals[idx : idx + 4]
        if len(row) < 4:
            break
        ident, mask, a, b = row
        if ident == 0 and mask == 0:
            break
        if (ident >> 16) == 0x12AB or ident in (0x00061CFA, 0x000E1CFA, 0x000F1CFA):
            print(
                f"{idx // 4:03d}: id=0x{ident:08x} mask=0x{mask:08x} "
                f"a=0x{a:08x} b=0x{b:08x}"
            )

    print(
        "\nNote: scale_tb gives the known 1920x1080 row, but 0x2d/0x31 "
        "also depend on runtime stream-state fields assembled by "
        "MZ0380_StartFirmware, so this script does not emit sendable packets."
    )

    print_packet_models()
    print_runtime_trace_targets()


def print_runtime_trace_targets():
    print("\n== Runtime trace targets for 0x2d/0x31 ==")
    print("Offsets are 32-bit word indexes relative to the named ARM context base.")
    for name, note, offsets in RUNTIME_TRACE_TARGETS:
        rendered = " ".join(f"+0x{o:03x}" for o in offsets)
        print(f"{name}:")
        print(f"  {note}")
        print(f"  words: {rendered}")


if __name__ == "__main__":
    main()
