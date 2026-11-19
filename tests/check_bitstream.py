"""Decode VideoToolbox fixtures and check random access or static luma refinement."""
import json
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

WIDTH, HEIGHT = 320, 180
COUNT, RECOVERY = 8, 5
MOVING, REPEATS = 12, 30


def access_units(path, count):
    with Path(path).open("rb") as stream:
        for _ in range(count):
            size, = struct.unpack(">I", stream.read(4))
            if not 0 < size <= 16 * 1024 * 1024:
                raise ValueError(f"invalid access unit size {size}")
            unit = stream.read(size)
            if len(unit) != size:
                raise ValueError("truncated access unit")
            yield unit
        if stream.read(1):
            raise ValueError("unexpected trailing data")


def check_random_access(au, codec):
    # Emulation prevention in NAL payloads makes Annex B start codes unambiguous.
    leading, *nals = re.split(b"\x00\x00(?:\x00)?\x01", au)
    if leading or not nals or any(not nal for nal in nals):
        raise ValueError("invalid Annex B access unit")
    if codec == "h264":
        types = [nal[0] & 31 for nal in nals]
        parameters, random_access = {7, 8}, {5}
    else:
        types = [(nal[0] >> 1) & 63 for nal in nals]
        parameters, random_access = {32, 33, 34}, set(range(16, 22))
    first = next((i for i, t in enumerate(types) if t in random_access), None)
    if first is None or not parameters <= set(types[:first]):
        raise ValueError(f"missing in-band parameters before random access slice: {types}")


def decode(stream, codec, count, required_keys):
    def run(executable, options):
        return subprocess.run([executable, "-v", "error", "-f", codec, "-i", "pipe:0", *options],
                              input=stream, stdout=subprocess.PIPE, check=True).stdout

    info = json.loads(run("ffprobe", ["-show_entries", "frame=width,height,pict_type,key_frame",
                                      "-show_frames", "-of", "json"]))
    frames = info["frames"]
    if len(frames) != count:
        raise ValueError(f"expected {count} frames, decoded {len(frames)}")
    for i, frame in enumerate(frames):
        if ((frame["width"], frame["height"]) != (WIDTH, HEIGHT) or
                frame["pict_type"] not in ("I", "P") or
                (i in required_keys and not frame["key_frame"])):
            raise ValueError(f"unexpected frame {i}: {frame}")

    raw = run("ffmpeg", ["-xerror", "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"])
    stride = WIDTH * HEIGHT * 3 // 2
    if len(raw) != len(frames) * stride:
        raise ValueError("incorrect decoded buffer size")
    return raw


def check_quadrants(raw, start):
    stride = WIDTH * HEIGHT * 3 // 2
    for i in range(COUNT - start):
        # Sample inside the changing grayscale quadrant in video-range YUV.
        shade = 30 + (i + start) * 25
        expected = round(16 + 219 * shade / 255)
        actual = raw[i * stride + (3 * HEIGHT // 4) * WIDTH + 3 * WIDTH // 4]
        if abs(actual - expected) > 8:
            raise ValueError(f"incorrect luma at frame {i + start}: {actual}, expected {expected}")


def check_recovery(path, codec):
    units = list(access_units(path, COUNT))
    for index in (0, RECOVERY):
        check_random_access(units[index], codec)
    full = decode(b"".join(units), codec, COUNT, {0, RECOVERY})
    recovered = decode(b"".join(units[RECOVERY:]), codec, COUNT - RECOVERY, {0})
    check_quadrants(full, 0)
    check_quadrants(recovered, RECOVERY)
    if not full.endswith(recovered):
        raise ValueError("recovery output differs from uninterrupted decoding")
    print(f"{codec}: decoded all {COUNT} frames without B pictures; fresh decoder reproduced recovery pixels")


def check_topoff(path, codec):
    units = list(access_units(path, MOVING + REPEATS))
    check_random_access(units[0], codec)
    raw = decode(b"".join(units), codec, len(units), {0})
    with Path(f"{path}.pgm").open("rb") as pgm:
        if pgm.readline() != b"P5\n":
            raise ValueError("missing topoff reference")
        header = pgm.readline().strip()
        if header not in (b"# encoder=hardware", b"# encoder=software"):
            raise ValueError(f"missing encoder backend in reference: {header!r}")
        backend = header.split(b"=")[1].decode()
        if pgm.readline().split() != [str(WIDTH).encode(), str(HEIGHT).encode()] or pgm.readline() != b"255\n":
            raise ValueError("incorrect reference geometry")
        reference = pgm.read()
    if len(reference) != WIDTH * HEIGHT:
        raise ValueError("incorrect reference luma size")

    def psnr(index):
        begin = index * (WIDTH * HEIGHT * 3 // 2)
        picture = raw[begin:begin + len(reference)]
        mse = sum((a - b) ** 2 for a, b in zip(reference, picture)) / len(reference)
        return 10 * math.log10(255 ** 2 / mse) if mse else math.inf

    before, after = psnr(MOVING - 1), psnr(MOVING + REPEATS - 1)
    gain = after - before
    print(f"{codec} ({backend}): luma PSNR {before:.2f} -> {after:.2f} dB, gain {gain:.2f} dB")
    if backend == "hardware" and (math.isnan(gain) or gain < 3.0):
        raise ValueError("hardware static topoff improved luma PSNR by less than 3 dB")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "topoff"):
        raise SystemExit("usage: check_bitstream.py h264|hevc stream.frames [topoff]")
    codec, path = sys.argv[1:3]
    if codec not in ("h264", "hevc"):
        raise ValueError(f"unknown FFmpeg demuxer {codec}")
    if len(sys.argv) == 4:
        check_topoff(path, codec)
    else:
        check_recovery(path, codec)
