# SPDX-License-Identifier: AGPL-3.0-or-later
"""Versioned bounded binary envelopes; JSON only for low-rate metadata/control.

Little-endian header: uint32 body_bytes, uint64 sequence, uint64 elapsed_ns,
uint64 utc_ns, uint32 opaque_receiver_id, uint8 event_type.
Sync body: two float64 radio ticks followed by two 14-byte Mode S messages.
MLAT body: float64 radio ticks followed by a 7- or 14-byte Mode S message.
Other payloads: allowlisted JSON. Radio ticks are never rescaled for replay.
"""
import json
import math
import struct

HEADER = struct.Struct('<IQQQIB')
SYNC = struct.Struct('<dd14s14s')
TICK = struct.Struct('<d')
KINDS = ('connect', 'disconnect', 'sync', 'mlat', 'seen', 'lost',
         'input_connected', 'input_disconnected', 'clock_reset', 'clock_jump',
         'heartbeat', 'rate_report', 'interest', 'result', 'status', 'sbs', 'connect_v2', 'client_output',
         'status_begin', 'status_row', 'status_end',
         'sbs_connect', 'sbs_output', 'sbs_disconnect', 'solution', 'location', 'random_choice', 'loop_turn',
         'status_aircraft_binary', 'random_choice_binary', 'rate_report_binary')
CODES = {kind: i for i, kind in enumerate(KINDS)}
_PYTHON_JSON_ENCODE = json.JSONEncoder(separators=(',', ':'), ensure_ascii=True,
                                       allow_nan=False).encode
# Exact version already present on the deployed server. Other environments keep
# the dependency-free implementation; no installation or broad upgrade required.
try:
    import ujson as _ujson
except ImportError:
    _ujson = None
if _ujson is not None and _ujson.__version__ == '5.12.0':
    from functools import partial
    _JSON_ENCODE = partial(_ujson.dumps, ensure_ascii=True, escape_forward_slashes=False,
                           allow_nan=False, reject_bytes=True)
    JSON_BACKEND = 'ujson-5.12.0'
else:
    _JSON_ENCODE = _PYTHON_JSON_ENCODE
    JSON_BACKEND = 'stdlib'


def encode(seq, elapsed, utc, kind, receiver, payload):
    if kind == 'sync':
        et, ot, em, om = payload
        if not math.isfinite(et) or not math.isfinite(ot):
            raise ValueError('nonfinite tick')
        em, om = bytes.fromhex(em), bytes.fromhex(om)
        if len(em) != 14 or len(om) != 14:
            raise ValueError('invalid sync size')
        body = SYNC.pack(et, ot, em, om)
    elif kind == 'mlat':
        t, m = payload
        if not math.isfinite(t):
            raise ValueError('nonfinite tick')
        m = bytes.fromhex(m)
        if len(m) not in (7, 14):
            raise ValueError('invalid message size')
        body = TICK.pack(t) + m
    elif kind == 'status_aircraft_binary':
        from .aircraft_binary import encode_body
        snapshot, table, key, row = payload
        if table != 'aircraft': raise ValueError('aircraft table')
        body = encode_body(snapshot, key, row)
    elif kind == 'random_choice_binary':
        from .random_binary import encode_body
        body = encode_body(payload)
    elif kind == 'rate_report_binary':
        from .rate_binary import encode_body
        body = encode_body(payload)
    else:
        body = _JSON_ENCODE(payload).encode('ascii')
    return HEADER.pack(len(body), seq, elapsed, utc, receiver, CODES[kind]) + body


def records(stream, *, maximum=65536):
    """Streaming decoder. Caller verifies manifest hashes and privacy permissions."""
    while True:
        header = stream.read(HEADER.size)
        if not header:
            return
        if len(header) != HEADER.size:
            raise ValueError('truncated header')
        size, seq, elapsed, utc, receiver, code = HEADER.unpack(header)
        if size + HEADER.size > maximum or code >= len(KINDS):
            raise ValueError('record bounds')
        body = stream.read(size)
        if len(body) != size:
            raise ValueError('truncated body')
        kind = KINDS[code]
        if kind == 'sync':
            if size != SYNC.size:
                raise ValueError('invalid sync size')
            et, ot, em, om = SYNC.unpack(body)
            if not math.isfinite(et) or not math.isfinite(ot):
                raise ValueError('nonfinite tick')
            payload = [et, ot, em.hex(), om.hex()]
        elif kind == 'mlat':
            if size not in (15, 22):
                raise ValueError('invalid message size')
            t, = TICK.unpack_from(body)
            if not math.isfinite(t):
                raise ValueError('nonfinite tick')
            payload = [t, body[TICK.size:].hex()]
        elif kind == 'status_aircraft_binary':
            from .aircraft_binary import decode_body
            if receiver != 0: raise ValueError('status receiver')
            payload = decode_body(body)
            kind = 'status_row'
        elif kind == 'random_choice_binary':
            from .random_binary import decode_body
            if receiver != 0: raise ValueError('choice receiver')
            payload = decode_body(body)
            kind = 'random_choice'
        elif kind == 'rate_report_binary':
            from .rate_binary import decode_body
            payload = decode_body(body)
            kind = 'rate_report'
        else:
            payload = json.loads(body)
        yield [seq, elapsed, utc, kind, receiver, payload]
