"""Telemetry v1, independent Python encoder used by the simulator and tests."""
import struct


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def frame(device=1, sequence=1, temperature=25.0, humidity=50.0):
    payload = struct.pack("<BHIhH", 1, device, sequence, round(temperature * 100), round(humidity * 100))
    return wrap(payload)


def wrap(payload):
    if len(payload) > 255:
        raise ValueError("payload exceeds one-byte length")
    body = bytes([len(payload)]) + payload
    return b"\xaa\x55" + body + struct.pack("<H", crc16(body))
