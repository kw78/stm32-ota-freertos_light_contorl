#!/usr/bin/env python3
"""
OTA 固件上传工具
用法: python ota_upload.py <serial_port> <firmware.bin> [baudrate]
示例: python ota_upload.py /dev/ttyUSB0 build/Debug/gcctest.bin 115200
"""

import serial
import struct
import sys
import time
import binascii

# 协议常量
PKT_HEADER    = 0xAA
CMD_OTA_START = 0x01
CMD_OTA_DATA  = 0x02
CMD_OTA_END   = 0x03
CMD_QUERY     = 0x10

CHUNK_SIZE    = 64   # 每包最大数据长度


def crc16_compute(data: bytes) -> int:
    """CRC16 校验"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def crc32_compute(data: bytes) -> int:
    """CRC32 校验"""
    return binascii.crc32(data) & 0xFFFFFFFF


def make_packet(cmd: int, data: bytes = b'') -> bytes:
    """
    构建协议包
    格式: | 0xAA | cmd | len | data | crc16(2字节) |
    """
    pkt_body = struct.pack('BB', cmd, len(data)) + data
    crc = crc16_compute(pkt_body)
    return bytes([PKT_HEADER]) + pkt_body + struct.pack('>H', crc)


def send_packet(ser: serial.Serial, cmd: int, data: bytes = b'', timeout: float = 5.0) -> bool:
    """发送一个包并等待 ACK"""
    pkt = make_packet(cmd, data)
    ser.write(pkt)
    return wait_ack(ser, timeout)


def wait_ack(ser: serial.Serial, timeout: float) -> bool:
    """等待 ACK (0x06) 或 NACK (0x15)，同时打印设备调试输出"""
    deadline = time.time() + timeout
    dbg_buf = b''
    while time.time() < deadline:
        if ser.in_waiting > 0:
            resp = ser.read(1)[0]
            if resp == 0x06:
                if dbg_buf:
                    print(f"  [设备] {dbg_buf.decode(errors='replace').strip()}")
                return True
            elif resp == 0x15:
                if dbg_buf:
                    print(f"  [设备] {dbg_buf.decode(errors='replace').strip()}")
                print("  收到 NACK")
                return False
            else:
                dbg_buf += bytes([resp])
        else:
            if dbg_buf:
                print(f"  [设备] {dbg_buf.decode(errors='replace').strip()}")
                dbg_buf = b''
            time.sleep(0.01)
    print("  等待 ACK 超时")
    return False
    return False


def main():
    if len(sys.argv) < 3:
        print(f"用法: python {sys.argv[0]} <serial_port> <firmware.bin> [baudrate]")
        print(f"示例: python {sys.argv[0]} /dev/ttyUSB0 build/Debug/gcctest.bin 115200")
        sys.exit(1)

    port = sys.argv[1]
    fw_path = sys.argv[2]
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

    # 读取固件
    with open(fw_path, 'rb') as f:
        fw_data = f.read()

    fw_size = len(fw_data)
    fw_crc = crc32_compute(fw_data)

    print(f"固件文件: {fw_path}")
    print(f"固件大小: {fw_size} bytes")
    print(f"CRC32:    0x{fw_crc:08X}")

    if fw_size > 56 * 1024:
        print("错误: 固件超过 56KB 限制")
        sys.exit(1)

    # 打开串口
    ser = serial.Serial(port, baud, timeout=1)
    print(f"串口: {port} @ {baud}")
    time.sleep(0.1)
    ser.reset_input_buffer()

    # ---- 发送 START 包 ----
    print("\n[1/3] 发送 OTA START...")
    start_data = struct.pack('<II', fw_size, fw_crc)
    if not send_packet(ser, CMD_OTA_START, start_data, timeout=30):
        print("START 失败，中止")
        ser.close()
        sys.exit(1)
    print("  START ACK 收到")

    # ---- 分块发送 DATA 包 ----
    print(f"\n[2/3] 发送 OTA DATA ({fw_size} bytes, {(fw_size + CHUNK_SIZE - 1) // CHUNK_SIZE} 包)...")
    seq = 0
    for offset in range(0, fw_size, CHUNK_SIZE):
        chunk = fw_data[offset:offset + CHUNK_SIZE]
        if not send_packet(ser, CMD_OTA_DATA, chunk, timeout=5):
            print(f"\n  DATA seq={seq} 失败，中止")
            ser.close()
            sys.exit(1)
        seq += 1
        progress = min(offset + CHUNK_SIZE, fw_size)
        print(f"\r  进度: {progress}/{fw_size} ({progress * 100 // fw_size}%)", end='', flush=True)

    print("\n  DATA 全部发送完成")

    # ---- 发送 END 包 ----
    print("\n[3/3] 发送 OTA END...")
    if not send_packet(ser, CMD_OTA_END, timeout=30):
        print("END 失败")
        ser.close()
        sys.exit(1)
    print("  END ACK 收到")

    print("\nOTA 上传完成! 设备将自动重启进入 Bootloader 搬运固件...")
    ser.close()


if __name__ == '__main__':
    main()
