#!/usr/bin/env python3
"""Capture the one-shot camera frame the firmware prints over the console.

VISION_DUMP_FRAME makes the device emit the JPEG the Himax actually saw, as
base64, bracketed by FRAME_BEGIN/FRAME_END. This reassembles it into a .jpg.

Why bother: every other signal about the camera is indirect. Detection scores
tell you the model is running, not what it is looking at — a covered lens, a
dark room and a sideways mount all read as "no detections". One picture settles
all three at once, and it is the same picture the vision model will be asked to
describe.

    python3 tools/grab_frame.py                  # wait for the next boot's frame
    python3 tools/grab_frame.py --reset          # reset the board and wait
    python3 tools/grab_frame.py -o /tmp/f.jpg

The frame is dumped once per boot, so --reset is usually what you want.
"""

import argparse
import base64
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not found. Try the IDF env:\n"
             "  ~/.espressif/python_env/idf5.5_py3.14_env/bin/python "
             + " ".join(sys.argv))


def find_console():
    """Pick the port that is actually talking ESP-IDF log lines.

    The Watcher exposes several USB serial endpoints — the ESP32 console, the
    Himax's own console at a different baud, and the bridge itself. Guessing by
    name is unreliable and gets you silence or mojibake, so probe instead.
    """
    for port in sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")):
        try:
            s = serial.Serial(port, 115200, timeout=0.5)
        except Exception:
            continue
        data = b""
        deadline = time.time() + 2.5
        while time.time() < deadline:
            data += s.read(512)
            if b") VISION:" in data or b") MAIN:" in data or b"FRAME" in data:
                s.close()
                return port
        s.close()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", help="serial port (default: probe)")
    ap.add_argument("-o", "--out", default="frame.jpg", help="output file")
    ap.add_argument("-t", "--timeout", type=int, default=90, help="seconds to wait")
    ap.add_argument("--reset", action="store_true",
                    help="reset the board first (the frame is dumped once per boot)")
    args = ap.parse_args()

    port = args.port or find_console()
    if port is None:
        sys.exit("no ESP32 console found — is the board attached?")
    print(f"listening on {port}", file=sys.stderr)

    s = serial.Serial(port, 115200, timeout=1)
    if args.reset:
        # Plain reset: IO0 stays high so it boots the app, not the loader.
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
        time.sleep(0.1)
        s.reset_input_buffer()
        print("reset", file=sys.stderr)

    chunks, collecting, expect = [], False, 0
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").rstrip()
        if not line:
            continue
        if "FRAME_BEGIN" in line:
            chunks, collecting = [], True
            try:
                expect = int(line.split("FRAME_BEGIN")[1].split()[0])
            except (IndexError, ValueError):
                expect = 0
            print(f"  {line.strip()}", file=sys.stderr)
            continue
        if "FRAME_END" in line:
            break
        if collecting and line.startswith("FRAME:"):
            chunks.append(line[len("FRAME:"):])
        elif not collecting and ") VISION:" in line:
            print(f"  {line.split(') ', 1)[-1]}", file=sys.stderr)
    s.close()

    if not chunks:
        sys.exit("no frame seen. Is VISION_DUMP_FRAME set to 1, and did the "
                 "board boot within the timeout? Try --reset.")

    b64 = "".join(chunks)
    if expect and len(b64) != expect:
        # Worth failing loudly: a short frame decodes to a truncated JPEG that
        # still opens, showing a partial image that looks like a camera fault.
        print(f"warning: got {len(b64)} base64 chars, expected {expect}",
              file=sys.stderr)
    raw = base64.b64decode(b64 + "=" * (-len(b64) % 4))
    with open(args.out, "wb") as fh:
        fh.write(raw)
    kind = "JPEG" if raw[:2] == b"\xff\xd8" else f"NOT a JPEG (starts {raw[:4].hex()})"
    print(f"wrote {args.out}: {len(raw)} bytes, {kind}")


if __name__ == "__main__":
    main()
