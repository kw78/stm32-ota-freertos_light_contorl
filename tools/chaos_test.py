#!/usr/bin/env python3
"""
断电/复位混沌测试台 —— P5 实验基础设施（真板）

做法：OTA 上传过程中在随机时刻注入复位（openocd `reset run`，模拟"任意
时刻断电"的 90% 近似——芯片复位但 SPI Flash 内容保持），然后断言设备在
时限内恢复到可 QUERY、且最终回到 IDLE。循环 N 次收集统计。

模型检查（model_check.py）证明了状态机逻辑上不变砖；本工具验证的是
"真实固件 + 真实 Flash 时序"下的同一主张——两者互补，缺一不可。

注意：
  * 串口单进程纪律（HANDOFF 教训）：整个 harness 只开一个串口，
    上传线程与恢复轮询共用，openocd 走独立 USB 通道
  * openocd 用 `reset run` 不用 halt（IWDG 运行期 8s，halt 超 3s 会咬人）
  * 真正的断电（Vcc 塌陷）需继电器硬件，属第二级；本脚本覆盖复位级

用法：
  python3 tools/chaos_test.py /dev/ttyUSB0 build/Debug/gcctest.bin \
      --iterations 20 [--recover-timeout 120] [--csv chaos.csv]
  python3 tools/chaos_test.py --dry-run --iterations 5    # 无硬件：自测 harness 逻辑
"""

from __future__ import annotations

import argparse
import csv
import random
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ota_tool import (CMD_OTA_DATA, CMD_OTA_END, CMD_OTA_START, CMD_QUERY,
                      ACK, NACK, CHUNK_SIZE, FW_MAX_SIZE,
                      crc32_compute, make_packet_v2, read_packet_v2,
                      start_payload, load_key)

try:
    import serial
except ImportError:
    serial = None


# ---------------------------------------------------------------- 复位注入

class OpenOcdResetter:
    """openocd `reset run`：独立进程、每次新起（stability > 速度）"""

    def __init__(self, cfg: str):
        self.cfg = cfg.split()

    def reset(self, timeout: float = 15.0) -> None:
        cmd = self.cfg + ['-c', 'init', '-c', 'reset run', '-c', 'shutdown']
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            raise RuntimeError(f"openocd 复位失败: {r.stderr.strip()[:300]}")


class FakeResetter:
    """dry-run 用：只打日志"""
    def reset(self, timeout: float = 15.0) -> None:
        log(f"  [dry-run] 注入复位")


def log(msg: str):
    print(msg, flush=True)


# ---------------------------------------------------------------- OTA 上传（静默版）

class UploadThread(threading.Thread):
    """后台上传；harness 在随机时刻复位设备，本线程会超时/异常退出——这是预期"""

    def __init__(self, link, fw: bytes, version: int):
        super().__init__(daemon=True)
        self.link = link
        self.fw = fw
        self.version = version
        self.last_seq = -1          # 已 ACK 的最大包号（断电时上传推进到哪）
        self.error = None
        self.finished = False

    def run(self):
        try:
            # v3：32B 签名 START（build_ts=当前时间，防降级地板之上）
            self.link.send(make_packet_v2(CMD_OTA_START,
                            start_payload(self.fw, self.version,
                                          int(time.time()), load_key())))
            if not self.link.wait_ack(30):
                self.error = 'START nacked'
                return
            for seq, off in enumerate(range(0, len(self.fw), CHUNK_SIZE)):
                self.link.send(make_packet_v2(CMD_OTA_DATA,
                                self.fw[off:off + CHUNK_SIZE], seq))
                if not self.link.wait_ack(5):
                    self.error = f'DATA seq={seq} 无响应（预期：复位已注入）'
                    return
                self.last_seq = seq
            self.link.send(make_packet_v2(CMD_OTA_END))
            if not self.link.wait_ack(30):
                self.error = 'END 无响应（预期：复位已注入）'
                return
            self.finished = True     # END 已确认，设备进入搬运/TESTING
        except Exception as e:       # 串口在复位风暴中断开等
            self.error = f'{type(e).__name__}: {e}'


# ---------------------------------------------------------------- 串口链路

class SerialLink:
    """唯一串口持有者（上传线程与主循环共用，天然无竞争：单写单读都在主线程调度）"""

    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=1)

    def send(self, data: bytes):
        self.ser.write(data)

    def wait_ack(self, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            n = self.ser.in_waiting
            if n:
                b = self.ser.read(n)
                if ACK in b:
                    return True
                if NACK in b:
                    return False
            else:
                time.sleep(0.01)
        return False

    def query(self, timeout: float = 3.0):
        """返回 QUERY 响应 dict 或 None"""
        try:
            self.ser.reset_input_buffer()
            self.ser.write(make_packet_v2(CMD_QUERY))
            resp = read_packet_v2(self.ser, CMD_QUERY, timeout)
        except Exception:
            return None
        if resp is None or len(resp) < 12:
            return None
        import struct
        ver, up = struct.unpack('<II', resp[4:12])
        return {'proto': resp[0], 'state': resp[1], 'golden': resp[2],
                'retry': resp[3], 'version': ver, 'uptime': up}

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


class FakeLink:
    """dry-run 用：模拟一台好设备（IDLE→PENDING→TESTING→IDLE 的 happy path）"""

    def __init__(self):
        self.state = 0
        self.t_boot = time.time()
        self._rx = bytearray()

    def send(self, data: bytes):
        self._rx += data

    def wait_ack(self, timeout: float) -> bool:
        time.sleep(0.002)
        return True

    def query(self, timeout: float = 3.0):
        # 模拟：复位后 12s 内完成 TESTING 确认 → IDLE
        state = 2 if (time.time() - self.t_boot) < 12 else 0
        return {'proto': 2, 'state': state, 'golden': 1, 'retry': 0,
                'version': 0x2BF95EA3, 'uptime': int(time.time() - self.t_boot)}

    def note_reset(self):
        self.t_boot = time.time()

    def close(self):
        pass


# ---------------------------------------------------------------- 主循环

def wait_responsive(link, resetter, timeout_s: float):
    """复位后轮询 QUERY 直到设备应答（Bootloader 不应答，App 起来才有响应）"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            resetter.reset(timeout=10)
        except Exception as e:
            log(f"  复位命令异常（重试）: {e}")
            time.sleep(2)
            continue
        for _ in range(8):
            if link.query(2.0):
                return time.time() - t0
            time.sleep(1.0)
    return None


def run_iteration(idx: int, link, resetter, fw: bytes, args) -> dict:
    log(f"\n=== 迭代 {idx + 1}/{args.iterations} ===")

    # 1. 干净起点
    boot_t = wait_responsive(link, resetter, timeout_s=60)
    if boot_t is None:
        return {'iter': idx, 'result': 'FAIL', 'reason': '基线复位后设备无响应'}
    log(f"  设备在线（{boot_t:.1f}s）")

    # 2. 随机延迟后注入复位（延迟范围覆盖整个上传窗口 + END 后的搬运窗口）
    est_upload_s = len(fw) / CHUNK_SIZE * 0.012 + 3     # 粗略：每包 ~12ms + 握手
    cut_delay = random.uniform(0.5, est_upload_s + 6.0)
    log(f"  计划在 OTA 开始后 {cut_delay * 1000:.0f} ms 注入复位")

    up = UploadThread(link, fw, args.version)
    up.start()
    time.sleep(cut_delay)
    t_cut = time.time()
    try:
        resetter.reset()
    except Exception as e:
        up.join(timeout=10)
        return {'iter': idx, 'result': 'ERROR', 'reason': f'复位失败: {e}'}
    up.join(timeout=15)
    if isinstance(link, FakeLink):
        link.note_reset()

    # 3. 恢复断言：时限内可 QUERY 且最终回 IDLE
    rec_t0 = time.time()
    first_resp = None
    while time.time() - rec_t0 < args.recover_timeout:
        q = link.query(3.0)
        if q and first_resp is None:
            first_resp = time.time() - rec_t0
            log(f"  恢复响应 @ {first_resp:.1f}s state={q['state']} golden={q['golden']}")
        if q and q['state'] == 0 and first_resp is not None and time.time() - rec_t0 > first_resp + 2:
            rec_ms = (time.time() - t_cut) * 1000
            log(f"  ✓ 回到 IDLE（版本 {q['version']:08X}），总恢复 {rec_ms / 1000:.1f}s")
            return {'iter': idx, 'result': 'PASS', 'cut_ms': cut_delay * 1000,
                    'last_seq': up.last_seq, 'upload_end': up.finished,
                    'recover_ms': rec_ms, 'state': q['state'], 'version': q['version']}
        time.sleep(2.0)

    q = link.query(3.0)
    return {'iter': idx, 'result': 'FAIL', 'cut_ms': cut_delay * 1000,
            'last_seq': up.last_seq, 'upload_end': up.finished,
            'reason': f"恢复超时 {args.recover_timeout}s（最后状态 {q}）"}


def main():
    ap = argparse.ArgumentParser(description='断电/复位混沌测试台')
    ap.add_argument('port', nargs='?', help='串口，如 /dev/ttyUSB0（dry-run 可省）')
    ap.add_argument('firmware', nargs='?', help='固件 .bin')
    ap.add_argument('--iterations', type=int, default=20)
    ap.add_argument('--recover-timeout', type=int, default=180)
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--version', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--openocd', default='openocd -f interface/stlink.cfg -f target/stm32f1x.cfg',
                    help='openocd 启动命令（接口/目标配置）')
    ap.add_argument('--seed', type=int, default=None, help='固定随机种子（复现）')
    ap.add_argument('--csv', default=None, help='结果 CSV 输出路径')
    ap.add_argument('--dry-run', action='store_true', help='无硬件自测 harness 逻辑')
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    if args.dry_run:
        link, resetter = FakeLink(), FakeResetter()
        fw = bytes(random.getrandbits(8) for _ in range(8 * 1024))   # 8KB 足以驱动时序
        args.iterations = min(args.iterations, 5)
        log("== DRY-RUN：模拟设备（happy path），验证 harness 状态机 ==")
    else:
        if not args.port or not args.firmware:
            ap.error('真机模式需要 port 和 firmware 参数')
        if serial is None:
            print('错误: 缺少 pyserial'); return 1
        fw = Path(args.firmware).read_bytes()
        if len(fw) > FW_MAX_SIZE:
            print(f'错误: 固件 {len(fw)}B 超限'); return 1
        link, resetter = SerialLink(args.port, args.baud), OpenOcdResetter(args.openocd)

    results = []
    try:
        for i in range(args.iterations):
            results.append(run_iteration(i, link, resetter, fw, args))
    finally:
        link.close()

    # ---- 汇总 ----
    passed = sum(1 for r in results if r['result'] == 'PASS')
    failed = sum(1 for r in results if r['result'] == 'FAIL')
    errors = sum(1 for r in results if r['result'] == 'ERROR')
    recs = [r['recover_ms'] for r in results if r.get('recover_ms')]
    log(f"\n==== 混沌测试汇总 ====")
    log(f"迭代: {len(results)}  PASS: {passed}  FAIL: {failed}  ERROR: {errors}")
    if recs:
        log(f"恢复时间: min {min(recs)/1000:.1f}s / 中位 {sorted(recs)[len(recs)//2]/1000:.1f}s"
            f" / max {max(recs)/1000:.1f}s")
    cut_points = [r.get('last_seq') for r in results if r.get('last_seq') is not None]
    if cut_points:
        log(f"断电时上传进度(seq): min {min(cut_points)} / max {max(cut_points)}")

    if args.csv:
        keys = sorted({k for r in results for k in r})
        with open(args.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(results)
        log(f"明细已写入 {args.csv}")

    return 1 if (failed or errors) else 0


if __name__ == '__main__':
    sys.exit(main())
