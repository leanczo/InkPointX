"""Inject the UTC build epoch used as the X4 clock's first-boot fallback."""

import os
import time

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

epoch_dir = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"])
epoch_file = os.path.join(epoch_dir, "fallback_clock_epoch.txt")
try:
    with open(epoch_file, "r", encoding="ascii") as source:
        build_epoch = int(source.read().strip())
except (FileNotFoundError, ValueError):
    build_epoch = int(time.time())
    os.makedirs(epoch_dir, exist_ok=True)
    with open(epoch_file, "w", encoding="ascii") as destination:
        destination.write(str(build_epoch))

env.Append(CPPDEFINES=[("CROSSPOINT_BUILD_EPOCH", build_epoch)])  # noqa: F821
print(f"CrossPoint fallback clock epoch: {build_epoch}")
