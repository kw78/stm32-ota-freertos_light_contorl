#!/usr/bin/env python3
"""
CI 尺寸闸门 + 固件清单生成

用法: python3 tools/ci_manifest.py <app.bin> <boot.bin>
- 校验 bootloader ≤ 10KB、app ≤ 52KB（v3 链接器分区）且 ≤ 56KB（OTA 上限）
- 生成 build/manifest.json（版本=git hash、大小、CRC32、SHA256）
退出码非 0 即失败（CI 直接红）
"""

import binascii
import hashlib
import json
import os
import subprocess
import sys

APP_LIMIT_LINKER = 52 * 1024
APP_LIMIT_OTA    = 56 * 1024
BOOT_LIMIT       = 10 * 1024


def git_hash() -> str:
    try:
        h = subprocess.check_output(
            ["git", "rev-parse", "--short=8", "HEAD"]).decode().strip()
        return h or "unknown"
    except Exception:
        return "unknown"


def firmware_info(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    return {
        "size": len(data),
        "crc32": f"{binascii.crc32(data) & 0xFFFFFFFF:08X}",
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    app_bin, boot_bin = sys.argv[1], sys.argv[2]

    app = firmware_info(app_bin)
    boot = firmware_info(boot_bin)

    ok = True
    if boot["size"] > BOOT_LIMIT:
        print(f"FAIL: bootloader {boot['size']}B > {BOOT_LIMIT}B (bootloader 分区)")
        ok = False
    if app["size"] > APP_LIMIT_LINKER:
        print(f"FAIL: app {app['size']}B > {APP_LIMIT_LINKER}B (链接器 54KB 分区)")
        ok = False
    if app["size"] > APP_LIMIT_OTA:
        print(f"FAIL: app {app['size']}B > {APP_LIMIT_OTA}B (OTA 56KB 上限)")
        ok = False
    if not ok:
        return 1

    manifest = {
        "version": git_hash(),
        "app": app,
        "bootloader": boot,
    }
    os.makedirs("build", exist_ok=True)
    with open("build/manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"PASS: app {app['size']}B ({app['size']*100//APP_LIMIT_LINKER}% of 52K), "
          f"boot {boot['size']}B ({boot['size']*100//BOOT_LIMIT}% of 10K)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
