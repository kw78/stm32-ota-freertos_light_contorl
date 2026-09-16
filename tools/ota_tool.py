#!/usr/bin/env python3
"""
OTA 固件工具 v2 —— upload / query，自动协商协议版本

用法:
  python3 ota_tool.py upload <serial_port> <firmware.bin> [baudrate] [--version N]
  python3 ota_tool.py query <serial_port> [baudrate]

示例:
  python3 ota_tool.py upload /dev/ttyUSB0 build/Debug/gcctest.bin 115200
  python3 ota_tool.py query  /dev/ttyUSB0

协议（v2）: | 0xAA | CMD | LEN | SEQ_L | SEQ_H | DATA | CRC16_H | CRC16_L |
            CRC16 覆盖 CMD+LEN+SEQ+DATA；DATA 包按 SEQ 幂等（可安全重传）
协议（v1）: | 0xAA | CMD | LEN | DATA | CRC16_H | CRC16_L |（旧固件，自动降级）
"""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
import time

try:
    import serial
except ImportError:          # 允许 --help / 无硬件环境下运行
    serial = None

PKT_HEADER    = 0xAA
CMD_OTA_START = 0x01
CMD_OTA_DATA  = 0x02
CMD_OTA_END   = 0x03
CMD_QUERY     = 0x10

ACK  = 0x06
NACK = 0x15

CHUNK_SIZE    = 64
FW_MAX_SIZE   = 56 * 1024
DATA_RETRY    = 3           # v2 单包最大重试次数（v1 无 SEQ 去重，不重试）


def crc16_compute(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def crc32_compute(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def make_packet_v2(cmd: int, data: bytes = b'', seq: int = 0) -> bytes:
    body = struct.pack('<BB', cmd, len(data)) + struct.pack('<H', seq) + data
    return bytes([PKT_HEADER]) + body + struct.pack('>H', crc16_compute(body))


def make_packet_v1(cmd: int, data: bytes = b'') -> bytes:
    body = struct.pack('<BB', cmd, len(data)) + data
    return bytes([PKT_HEADER]) + body + struct.pack('>H', crc16_compute(body))


def make_packet(proto: int, cmd: int, data: bytes = b'', seq: int = 0) -> bytes:
    return make_packet_v2(cmd, data, seq) if proto == 2 else make_packet_v1(cmd, data)


def read_packet_v2(ser: serial.Serial, want_cmd: int, timeout: float):
    """读一个完整的 v2 响应帧（QUERY 应答用），返回 data 或 None"""
    deadline = time.time() + timeout
    buf = b''
    while time.time() < deadline:
        if ser.in_waiting > 0:
            buf += ser.read(ser.in_waiting)
            # 找帧头
            while len(buf) >= 8:      # 最小帧: AA CMD LEN SEQ(2) (0 data) CRC(2)
                idx = buf.find(bytes([PKT_HEADER, want_cmd]))
                if idx < 0:
                    buf = buf[-1:]
                    break
                buf = buf[idx:]
                if len(buf) < 3:
                    break
                ln = buf[2]
                total = 3 + 2 + ln + 2
                if len(buf) < total:
                    break
                body = buf[3:3 + 2 + ln]
                crc_rx = (buf[total - 2] << 8) | buf[total - 1]
                if crc16_compute(bytes([buf[1], buf[2]]) + body) == crc_rx:
                    return body[2:]           # 去掉 SEQ
                buf = buf[1:]
        else:
            time.sleep(0.01)
    return None


def wait_ack(ser: serial.Serial, timeout: float) -> bool:
    """等待 ACK/NACK，透传设备调试文本"""
    deadline = time.time() + timeout
    dbg = b''
    while time.time() < deadline:
        if ser.in_waiting > 0:
            b = ser.read(1)[0]
            if b == ACK:
                if dbg:
                    print(f"  [设备] {dbg.decode(errors='replace').strip()}")
                return True
            elif b == NACK:
                if dbg:
                    print(f"  [设备] {dbg.decode(errors='replace').strip()}")
                return False
            else:
                dbg += bytes([b])
        else:
            if dbg:
                print(f"  [设备] {dbg.decode(errors='replace').strip()}")
                dbg = b''
            time.sleep(0.01)
    print("  等待 ACK 超时")
    return False


def probe_protocol(ser: serial.Serial) -> int:
    """QUERY 探测：v2 固件回状态包，v1/无响应 → 降级 v1"""
    ser.reset_input_buffer()
    ser.write(make_packet_v2(CMD_QUERY))
    resp = read_packet_v2(ser, CMD_QUERY, timeout=1.0)
    if resp is not None and len(resp) >= 12:
        return 2
    return 1


def print_query(resp: bytes):
    proto, state, golden, retry = resp[0], resp[1], resp[2], resp[3]
    version, uptime = struct.unpack('<II', resp[4:12])
    state_names = {0: 'IDLE', 1: 'PENDING', 2: 'TESTING'}
    print(f"  协议版本 : v{proto}")
    print(f"  固件版本 : {version:08X}")
    print(f"  升级状态 : {state_names.get(state, hex(state))}")
    print(f"  金固件   : {'可用' if golden else '无（首次升级尚无回滚保护）'}")
    print(f"  回滚计数 : {retry}")
    print(f"  已运行   : {uptime} s")


def cmd_query(args) -> int:
    if serial is None:
        print("错误: 缺少 pyserial（pip install pyserial）")
        return 1
    ser = serial.Serial(args.port, args.baud, timeout=1)
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.write(make_packet_v2(CMD_QUERY))
    resp = read_packet_v2(ser, CMD_QUERY, timeout=2.0)
    if resp is None:
        print("设备无响应（v1 固件或未运行）")
        ser.close()
        return 1
    print_query(resp)
    ser.close()
    return 0


def cmd_upload(args) -> int:
    if serial is None:
        print("错误: 缺少 pyserial（pip install pyserial）")
        return 1
    with open(args.firmware, 'rb') as f:
        fw = f.read()
    if len(fw) > FW_MAX_SIZE:
        print(f"错误: 固件 {len(fw)}B 超过 {FW_MAX_SIZE}B 限制")
        return 1

    fw_crc = crc32_compute(fw)
    print(f"固件: {args.firmware} ({len(fw)} B, CRC32 {fw_crc:08X})")

    ser = serial.Serial(args.port, args.baud, timeout=1)
    time.sleep(0.1)
    ser.reset_input_buffer()

    proto = probe_protocol(ser)
    print(f"协议协商: v{proto}" + ("（老固件，本次升级即完成换代）" if proto == 1 else ""))
    if args.proto != 'auto' and int(args.proto) != proto:
        print(f"提示: --proto v{args.proto} 与探测结果不符，按探测结果 v{proto} 继续")

    # ---- START ----
    print("\n[1/3] OTA START ...")
    if proto == 2:
        start_data = struct.pack('<III', len(fw), fw_crc, args.version)
    else:
        start_data = struct.pack('<II', len(fw), fw_crc)
    ser.write(make_packet(proto, CMD_OTA_START, start_data))
    if not wait_ack(ser, 30):
        print("START 失败，中止")
        ser.close()
        return 1

    # ---- DATA ----
    total = (len(fw) + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"\n[2/3] OTA DATA ({total} 包) ...")
    for seq, offset in enumerate(range(0, len(fw), CHUNK_SIZE)):
        chunk = fw[offset:offset + CHUNK_SIZE]
        ok = False
        attempts = 0
        while not ok:
            attempts += 1
            ser.write(make_packet(proto, CMD_OTA_DATA, chunk, seq))
            ok = wait_ack(ser, 5)
            if not ok and proto == 1:
                break               # v1 无序号去重，重传不安全，直接失败
            if not ok and attempts >= DATA_RETRY:
                break
        if not ok:
            print(f"\nDATA seq={seq} 失败（v1 不可重试 / v2 重试 {DATA_RETRY} 次耗尽），中止")
            ser.close()
            return 1
        done = min(offset + CHUNK_SIZE, len(fw))
        print(f"\r  进度: {done}/{len(fw)} ({done * 100 // len(fw)}%)", end='', flush=True)
    print()

    # ---- END ----
    print("\n[3/3] OTA END ...")
    ser.write(make_packet(proto, CMD_OTA_END))
    if not wait_ack(ser, 30):
        print("END 失败（固件 CRC 不匹配或设备忙）")
        ser.close()
        return 1
    print("上传完成! 设备已复位，Bootloader 搬运 → 10s 健康观察后自动确认")
    print("提示: 稍后可用 `ota_tool.py query` 查看状态（TESTING→IDLE 即确认成功）")
    ser.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description='STM32 OTA 固件工具 (v1/v2 自适应)')
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_up = sub.add_parser('upload', help='上传固件')
    p_up.add_argument('port', help='串口, 如 /dev/ttyUSB0')
    p_up.add_argument('firmware', help='固件 .bin 路径')
    p_up.add_argument('baud', nargs='?', type=int, default=115200)
    p_up.add_argument('--version', type=lambda x: int(x, 0), default=0,
                      help='固件版本号（默认 0）')
    p_up.add_argument('--proto', choices=['auto', '1', '2'], default='auto')
    p_up.set_defaults(func=cmd_upload)

    p_q = sub.add_parser('query', help='查询设备状态')
    p_q.add_argument('port', help='串口')
    p_q.add_argument('baud', nargs='?', type=int, default=115200)
    p_q.set_defaults(func=cmd_query)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == '__main__':
    main()
