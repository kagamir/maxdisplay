"""Independently check the VideoToolbox fixture and a clean restart at the forced keyframe."""
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

WIDTH, HEIGHT = 320, 180
COUNT, RECOVERY = 8, 5


def access_units(path):
    with Path(path).open("rb") as stream:
        for _ in range(COUNT):
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


def decode(stream, codec, start):
    def run(executable, options):
        return subprocess.run([executable, "-v", "error", "-f", codec, "-i", "pipe:0", *options],
                              input=stream, stdout=subprocess.PIPE, check=True).stdout

    info = json.loads(run("ffprobe", ["-show_entries", "frame=width,height,pict_type,key_frame",
                                      "-show_frames", "-of", "json"]))
    frames = info["frames"]
    if len(frames) != COUNT - start or not frames[0]["key_frame"]:
        raise ValueError(f"incorrect number of frames or missing sync at {start}")
    for i, frame in enumerate(frames, start):
        if ((frame["width"], frame["height"]) != (WIDTH, HEIGHT) or
                frame["pict_type"] not in ("I", "P") or
                (i == RECOVERY and not frame["key_frame"])):
            raise ValueError(f"unexpected frame {i}: {frame}")

    raw = run("ffmpeg", ["-xerror", "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"])
    stride = WIDTH * HEIGHT * 3 // 2
    if len(raw) != len(frames) * stride:
        raise ValueError("incorrect decoded buffer size")
    for i in range(len(frames)):
        # Sample inside the changing grayscale quadrant in video-range YUV.
        shade = 30 + (i + start) * 25
        expected = round(16 + 219 * shade / 255)
        actual = raw[i * stride + (3 * HEIGHT // 4) * WIDTH + 3 * WIDTH // 4]
        if abs(actual - expected) > 8:
            raise ValueError(f"incorrect luma at frame {i + start}: {actual}, expected {expected}")
    return raw


if __name__ == "__main__":
    codec, path = sys.argv[1:]
    if codec not in ("h264", "hevc"):
        raise ValueError(f"unknown FFmpeg demuxer {codec}")
    units = list(access_units(path))
    for index in (0, RECOVERY):
        check_random_access(units[index], codec)
    full = decode(b"".join(units), codec, 0)
    recovered = decode(b"".join(units[RECOVERY:]), codec, RECOVERY)
    if not full.endswith(recovered):
        raise ValueError("recovery output differs from uninterrupted decoding")
    print(f"{codec}: decoded all {COUNT} frames without B pictures; fresh decoder reproduced recovery pixels")
