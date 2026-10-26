#!/usr/bin/env python3
"""Check Rule 14: Linux host dynamically uses only the system glibc loader/libs."""
import pathlib
import re
import subprocess
import sys

allowed = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1"}

host = pathlib.Path(sys.argv[1])
dynamic = subprocess.check_output(["readelf", "-dW", str(host)], text=True)
needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
if "libc.so.6" not in needed:
    raise SystemExit("Linux host must dynamically link libc.so.6")
bad = [n for n in needed if n not in allowed and not re.fullmatch(r"ld-linux[^/]*\.so(?:\.[0-9]+)*", n)]
if bad:
    raise SystemExit("unexpected dynamic libraries: " + ", ".join(bad))
interp = subprocess.check_output(["readelf", "-lW", str(host)], text=True)
if not re.search(r"Requesting program interpreter: .*ld-linux.*\.so", interp):
    raise SystemExit("Linux host has no glibc dynamic loader")
versions = [tuple(map(int, x.split("."))) for x in re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", subprocess.check_output(["readelf", "--version-info", "-W", str(host)], text=True))]
if versions and max(versions) > (2, 35):
    raise SystemExit("host requires glibc newer than 2.35")
print("dynamic glibc linkage passed: " + ", ".join(needed))
