#!/usr/bin/env python3
"""
HIL（硬件在环）验收 —— 真板全链路回归，供 CI self-hosted runner 或本地执行

验收剧本（对应 README 的验收清单，一次跑完）：
  0.（可选）openocd 全新烧录 boot + app → 已知基线
  1. QUERY 探测 v2 协议在线
  2. OTA 上传 good 镜像 → 等待 安装→TESTING→10s 确认→金备份→IDLE
  3. OTA 上传 bad 镜像（BAD_FW_TEST 构建：banner 后写空指针）
     → 崩溃循环 → Bootloader IWDG 计数回滚 → IDLE
  4. 断言：回滚后版本 == good 版本；黑匣子存在 REC_FAULT 崩溃现场
  5. 汇总 PASS/FAIL（退出码）

用法：
  python3 tools/hil_accept.py --port /dev/ttyUSB0 \
      --good build/Debug/gcctest.bin --bad build/badfw/gcctest.bin \
      [--flash]                      # 先用 openocd 烧录全新 boot+app 基线
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ota_tool import (ACK, CHUNK_SIZE, CMD_GET_LOG, CMD_OTA_DATA, CMD_OTA_END,
                      CMD_OTA_START, CMD_QUERY, NACK, crc32_compute,
                      make_packet_v2, read_packet_v2)

try:
    import serial
except ImportError:
    serial = None

STATE_NAMES = {0: 'IDLE', 1: 'PENDING', 2: 'TESTING'}


class Dev:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=1)

    def send(self, pkt): self.ser.write(pkt)

    def wait_ack(self, timeout=10):
        t0 = time.time()
        while time.time() - t0 < timeout:
            n = self.ser.in_waiting
            if n:
                b = self.ser.read(n)
                if ACK in b: return True
                if NACK in b: return False
            else:
                time.sleep(0.01)
        return False

    def query(self, timeout=3.0):
        self.ser.reset_input_buffer()
        self.ser.write(make_packet_v2(CMD_QUERY))
        r = read_packet_v2(self.ser, CMD_QUERY, timeout)
        if r is None or len(r) < 12: return None
        ver, up = struct.unpack('<II', r[4:12])
        return {'proto': r[0], 'state': r[1], 'golden': r[2],
                'retry': r[3], 'version': ver, 'uptime': up}

    def wait_state(self, target, timeout, desc=''):
        """轮询直到 state==target（或超时），返回最后的 QUERY 或 None"""
        t0 = time.time()
        last = None
        while time.time() - t0 < timeout:
            q = self.query(3.0)
            if q:
                last = q
                if q['state'] == target:
                    return q
            time.sleep(2.0)
        print(f"    等待 {STATE_NAMES.get(target, target)} 超时（{desc}），最后: {last}")
        return None

    def upload(self, fw: bytes, version: int) -> bool:
        """v2 上传（带回退重试的简化版，与 ota_tool 语义一致）"""
        self.ser.reset_input_buffer()
        self.send(make_packet_v2(CMD_OTA_START,
                  struct.pack('<III', len(fw), crc32_compute(fw), version)))
        if not self.wait_ack(30):
            return False
        for seq, off in enumerate(range(0, len(fw), CHUNK_SIZE)):
            for attempt in range(3):
                self.send(make_packet_v2(CMD_OTA_DATA, fw[off:off + CHUNK_SIZE], seq))
                if self.wait_ack(5):
                    break
            else:
                return False
        self.send(make_packet_v2(CMD_OTA_END))
        return self.wait_ack(30)

    def blackbox_scan_tail(self, slots=128):
        """扫黑匣子最后 N 个槽，返回解码记录列表（kind 字段）"""
        recs = []
        for idx in range(0, slots):
            self.ser.write(make_packet_v2(CMD_GET_LOG, struct.pack('<BH', 1, idx)))
            r = read_packet_v2(self.ser, CMD_GET_LOG, timeout=2.0)
            if r and len(r) >= 32:
                recs.append(struct.unpack('<HHH', r[:6]))   # magic, kind, seq
        return recs


def flash_baseline() -> bool:
    """openocd 烧录 bootloader + app（README 同款命令）"""
    cmds = ['openocd', '-f', 'interface/stlink.cfg', '-f', 'target/stm32f1x.cfg',
            '-c', 'init', '-c', 'reset halt',
            '-c', 'flash erase_address 0x08000000 0x10000',
            '-c', 'flash write_image /tmp/hil_boot.bin 0x08000000 bin',
            '-c', 'flash write_image /tmp/hil_good.bin 0x08002000 bin',
            '-c', 'reset run', '-c', 'shutdown']
    r = subprocess.run(cmds, capture_output=True, text=True, timeout=120)
    return r.returncode == 0


def step(n, desc):
    print(f"\n[{n}] {desc}", flush=True)


def main():
    ap = argparse.ArgumentParser(description='HIL 真板验收')
    ap.add_argument('--port', required=True)
    ap.add_argument('--good', required=True, help='good 固件 .bin')
    ap.add_argument('--bad', required=True, help='BAD_FW_TEST 固件 .bin')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--version', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--bad-version', type=lambda x: int(x, 0), default=0xBADF0001)
    ap.add_argument('--flash', action='store_true', help='先 openocd 烧录全新基线')
    args = ap.parse_args()

    if serial is None:
        print('错误: 缺少 pyserial'); return 1

    good = Path(args.good).read_bytes()
    bad = Path(args.bad).read_bytes()

    results = []

    def check(name, ok, detail=''):
        results.append((name, ok))
        print(f"    {'✓' if ok else '✗'} {name} {detail}")

    if args.flash:
        step(0, 'openocd 烧录全新基线 (boot+app)')
        import shutil
        for src, dst in [('bootloader/build/bootloader.elf', '/tmp/hil_boot.bin'),
                         (args.good, '/tmp/hil_good.bin')]:
            if src.endswith('.elf'):
                r = subprocess.run(['arm-none-eabi-objcopy', '-O', 'binary', src, dst])
                if r.returncode != 0:
                    print('objcopy 失败'); return 1
            else:
                shutil.copy(src, dst)
        check('烧录成功', flash_baseline())

    dev = Dev(args.port, args.baud)
    time.sleep(0.3)

    step(1, 'QUERY 探测 v2 协议')
    q = dev.query()
    check('v2 在线', q is not None and q['proto'] == 2, f"{q}")

    step(2, f'OTA 上传 good 镜像 ({len(good)}B)')
    check('上传完成', dev.upload(good, args.version))
    q = dev.wait_state(0, timeout=90, desc='安装+确认+金备份')
    check('回到 IDLE（确认成功）', q is not None, f"{q}")
    ver_good = q['version'] if q else None
    check('金固件已建立', bool(q and q['golden']))

    step(3, f'OTA 上传 bad 镜像 BADF0001（崩溃→自动回滚）')
    check('上传完成', dev.upload(bad, args.bad_version))
    print('    等待崩溃循环 → IWDG ×4 → Bootloader 回滚（最长 150s）...')
    q = dev.wait_state(0, timeout=150, desc='坏固件回滚')
    check('自动回滚到 IDLE', q is not None, f"{q}")
    check('版本恢复为 good', q is not None and q['version'] == ver_good,
          f"0x{q['version']:08X}" if q else '')

    step(4, '黑匣子取证')
    recs = dev.blackbox_scan_tail()
    kinds = [k for (_, k, _) in recs]
    check('存在 REC_FAULT 崩溃现场', 2 in kinds, f"kinds={sorted(set(kinds))}")

    dev.ser.close()

    print('\n==== HIL 验收汇总 ====')
    passed = sum(1 for _, ok in results if ok)
    for name, ok in results:
        print(f"  {'✓' if ok else '✗'} {name}")
    print(f"通过 {passed}/{len(results)}")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
