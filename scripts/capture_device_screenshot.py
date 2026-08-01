#!/usr/bin/env python3
"""Capture the current XTEINK framebuffer over the firmware serial console."""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import serial
from PIL import Image


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Serial port, for example /dev/cu.usbmodem1101")
    parser.add_argument("output", type=Path, help="Destination PNG or BMP path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument(
        "--command",
        help="Optional firmware command to run before capture, without the CMD: prefix",
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=2.5,
        help="Seconds to wait after the optional command before capturing",
    )
    parser.add_argument(
        "--prewait",
        type=float,
        default=0.0,
        help="Seconds to keep the serial connection open before sending any command",
    )
    return parser.parse_args()


def read_framebuffer(
    port: str,
    baud: int,
    timeout: float,
    command: str | None = None,
    settle: float = 0.0,
    prewait: float = 0.0,
) -> bytes:
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = baud
    connection.timeout = 0.1
    connection.dtr = False
    connection.rts = False
    connection.open()
    try:
        time.sleep(max(0.0, prewait))
        connection.reset_input_buffer()
        if command:
            connection.write(f"CMD:{command}\n".encode("ascii"))
            connection.flush()
            time.sleep(max(0.0, settle))
            connection.reset_input_buffer()

        deadline = time.monotonic() + timeout
        connection.write(b"CMD:SCREENSHOT\n")
        connection.flush()

        size = None
        while time.monotonic() < deadline:
            line = connection.readline()
            if line.startswith(b"SCREENSHOT_START:"):
                size = int(line.split(b":", 1)[1])
                break
        if size is None:
            raise TimeoutError("The device did not start a screenshot transfer")

        data = bytearray()
        while len(data) < size and time.monotonic() < deadline:
            chunk = connection.read(size - len(data))
            if chunk:
                data.extend(chunk)
        if len(data) != size:
            raise TimeoutError(f"Received {len(data)} of {size} framebuffer bytes")
        return bytes(data)
    finally:
        connection.close()


def main() -> None:
    args = parse_args()
    framebuffer = read_framebuffer(
        args.port,
        args.baud,
        args.timeout,
        command=args.command,
        settle=args.settle,
        prewait=args.prewait,
    )
    image = Image.frombytes("1", (800, 480), framebuffer).transpose(Image.Transpose.ROTATE_270)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(f"Captured {image.width}x{image.height} framebuffer to {args.output}")


if __name__ == "__main__":
    main()
