# SPDX-License-Identifier: AGPL-3.0-or-later
"""Lossless bounded status-row encoding; decode to the existing status contract."""
import struct
from .schema import number, hexstr
from .status import _python_aircraft, counter

FIELDS = ('elapsed_seen', 'interesting', 'allow_mlat', 'tracking', 'sync_interest',
          'mlat_interest', 'adsb_seen', 'mlat_message_count', 'mlat_result_count',
          'mlat_kalman_count', 'sync_count_1min', 'sync_bad_percent', 'last_result',
          'lat', 'lon', 'alt', 'heading', 'speed')
HEADER = struct.Struct('<Q6sI')
REQUIRED = (1 << 12)-1
TRACKING = 1 << 18


def encode_body(snapshot, key, row):
    if type(snapshot) is not int or not 0 < snapshot < 2**64:
        raise ValueError('snapshot bound')
    hexstr(key, (6,))
    row = _python_aircraft(row, counter)
    if row['icao'] != key: raise ValueError('aircraft key mismatch')
    flags = sum(1 << i for i, name in enumerate(FIELDS) if name in row)
    if 'tracking_receivers' in row: flags |= TRACKING
    body = bytearray(HEADER.pack(snapshot, key.encode('ascii'), flags))
    for name in FIELDS:
        if name in row:
            value = row[name]
            body += (b'\x00'+struct.pack('<q', value) if type(value) is int
                     else b'\x01'+struct.pack('<d', value))
    if flags & TRACKING:
        values = row['tracking_receivers']
        body += struct.pack('<I', len(values))
        for value in values: body += struct.pack('<I', value)
    return bytes(body)


def decode_body(body):
    if len(body) < HEADER.size: raise ValueError('short aircraft record')
    snapshot, key, flags = HEADER.unpack_from(body)
    if not snapshot or flags & ~((1 << 19)-1) or flags & REQUIRED != REQUIRED:
        raise ValueError('aircraft flags')
    key = key.decode('ascii'); hexstr(key, (6,)); row = {'icao': key}
    offset = HEADER.size
    for i, name in enumerate(FIELDS):
        if flags & (1 << i):
            if offset+9 > len(body) or body[offset] not in (0, 1):
                raise ValueError('aircraft numeric bounds')
            value, = struct.unpack_from('<q' if body[offset] == 0 else '<d', body, offset+1)
            row[name] = number(value); offset += 9
    if flags & TRACKING:
        if offset+4 > len(body): raise ValueError('aircraft tracking bounds')
        count, = struct.unpack_from('<I', body, offset); offset += 4
        if count > 4096 or offset+4*count != len(body): raise ValueError('aircraft tracking bounds')
        row['tracking_receivers'] = [counter(x[0]) for x in struct.iter_unpack('<I', body[offset:])]
        offset += count*4
    if offset != len(body): raise ValueError('aircraft trailing bytes')
    return [snapshot, 'aircraft', key, row]
