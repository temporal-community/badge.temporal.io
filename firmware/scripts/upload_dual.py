"""
PlatformIO extra_script (post:): flash all connected ESP32-S3 badges concurrently.

Overrides the Upload target so that clicking "Upload" in PlatformIO discovers
all /dev/cu.usbmodem* ports and runs esptool against each in parallel.
"""

import subprocess
import threading
import sys
import os
import glob as globmod

Import("env")  # noqa: F821


def find_esp32s3_ports():
    patterns = ["/dev/cu.usbmodem*", "/dev/ttyACM*"]
    ports = []
    for pat in patterns:
        ports.extend(globmod.glob(pat))
    return sorted(ports)


def flash_one(port, flash_images, results, idx):
    """flash_images is a list of (offset, filepath) tuples."""
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", port,
        "--baud", "921600",
        "--before=default_reset",
        "--after=hard_reset",
        "write_flash",
        "-z",
        "--flash_mode", "dio",
        "--flash_size", "detect",
    ]
    for offset, path in flash_images:
        cmd.extend([offset, path])

    env_copy = os.environ.copy()
    env_copy["PATH"] = os.path.dirname(sys.executable) + ":" + env_copy.get("PATH", "")

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=90,
                              env=env_copy)
        results[idx] = (port, proc.returncode == 0, proc.stdout + proc.stderr)
    except subprocess.TimeoutExpired:
        results[idx] = (port, False, "Timed out after 90s")
    except Exception as e:
        results[idx] = (port, False, str(e))


def on_upload(source, target, env):
    ports = find_esp32s3_ports()
    if not ports:
        print("\n  ✗  No ESP32-S3 ports found. Are badges plugged in?\n")
        return 1

    build_dir = env.subst("$BUILD_DIR")
    # Replicate what PlatformIO's default ESP32 upload does:
    # bootloader @ 0x0, partitions @ 0x8000, app @ 0x10000
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware = str(source[0])

    flash_images = []
    if os.path.isfile(bootloader):
        flash_images.append(("0x0", bootloader))
    if os.path.isfile(partitions):
        flash_images.append(("0x8000", partitions))
    flash_images.append(("0x10000", firmware))

    print(f"\n  ══ Dual Flash: {len(ports)} badge(s) detected ══")
    for p in ports:
        print(f"     • {p}")
    print(f"  Images: {', '.join(os.path.basename(f) + '@' + o for o, f in flash_images)}")
    print()

    results = [None] * len(ports)
    threads = []
    for i, port in enumerate(ports):
        t = threading.Thread(target=flash_one,
                             args=(port, flash_images, results, i))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    failed = 0
    for port, success, output in results:
        status = "✓" if success else "✗"
        print(f"  {status}  {port}")
        if not success:
            failed += 1
            for line in output.strip().split("\n")[-5:]:
                print(f"       {line}")

    print()
    if failed:
        print(f"  ✗  {failed}/{len(ports)} badge(s) failed.\n")
        return 1
    print(f"  ✓  All {len(ports)} badge(s) flashed successfully.\n")
    return 0


env.Replace(UPLOADCMD=on_upload)  # noqa: F821
