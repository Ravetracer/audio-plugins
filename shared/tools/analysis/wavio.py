"""Minimal WAV reader/writer: 16/24/32-bit PCM and 32-bit float, any channel count."""
import struct
import numpy as np


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a RIFF/WAVE file: %s' % path)
    pos = 12
    fmt = None
    raw = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise ValueError('missing fmt or data chunk: %s' % path)
    tag, channels, sr, _, _, bits = fmt

    if tag == 3 and bits == 32:
        x = np.frombuffer(raw, dtype='<f4').astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif bits == 24:
        n = len(raw) // 3
        b = np.frombuffer(raw[:n * 3], dtype=np.uint8).reshape(n, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    elif bits == 32:
        x = np.frombuffer(raw, dtype='<i4').astype(np.float64) / 2147483648.0
    else:
        raise ValueError('unsupported format tag=%d bits=%d' % (tag, bits))

    x = x[:len(x) // channels * channels].reshape(-1, channels)
    return x, sr


def to_mono(x):
    return x.mean(axis=1)
