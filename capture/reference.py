# SPDX-License-Identifier: AGPL-3.0-or-later
"""Strict allowlist for already-serialized feeder output, never raw handshakes."""
import re
from numbers import Real
from .schema import number, hexstr

OLD = frozenset(('@', 'addr', 'lat', 'lon', 'alt', 'callsign', 'squawk',
                 'hdop', 'vdop', 'tdop', 'gdop', 'nstations'))
ECEF = frozenset(('@', 'addr', 'n', 'nd', 'ecef', 'cov'))
OPTIONAL = frozenset(('nsvel', 'ewvel', 'vrate'))
OUTPUTS = frozenset(('start_sending', 'stop_sending', 'result', 'stats', 'heartbeat'))


def server_number(value):
    """Normalize trusted numerical kernel scalars; never coerce text or bools.

    NumPy round(float64) remains float64 before ujson serializes it. Decoder
    validation continues to use schema.number's exact builtin-type allowlist.
    """
    if type(value) in (int, float):
        return number(value)
    if isinstance(value, Real) and not isinstance(value, bool):
        return number(float(value))
    raise ValueError('invalid server number')


def result(value, scalar=number):
    if type(value) is not dict:
        raise ValueError('result shape')
    keys = value.keys()
    if keys == OLD:
        out = {k: scalar(value[k]) for k in OLD - {'addr', 'callsign', 'squawk'}}
        for key, pattern in (('callsign', r'[A-Z0-9 _#?]{0,8}'), ('squawk', r'[0-7]{4}')):
            text = value[key]
            if text is not None and (type(text) is not str or not re.fullmatch(pattern, text)):
                raise ValueError('aircraft text bounds')
            out[key] = text
    elif ECEF <= keys <= ECEF | OPTIONAL:
        out = {k: scalar(value[k]) for k in keys - {'addr', 'ecef', 'cov'}}
        for key, size in (('ecef', 3), ('cov', 6)):
            values = value[key]
            if key == 'cov' and values is None:
                out[key] = None
            else:
                if type(values) not in (list, tuple) or len(values) != size:
                    raise ValueError('result vector shape')
                out[key] = tuple(scalar(x) for x in values)
    else:
        raise ValueError('result fields')
    out['addr'] = hexstr(value['addr'], (6,))
    return out


def output(message, scalar=number):
    if type(message) is not dict or len(message) != 1:
        raise ValueError('client output shape')
    kind, value = next(iter(message.items()))
    if kind in ('start_sending', 'stop_sending'):
        if type(value) not in (tuple, list) or len(value) > 4096:
            raise ValueError('interest list bound')
        value = tuple(hexstr(x, (6,)) for x in value)
    elif kind == 'result':
        value = result(value, scalar)
    elif kind in ('stats', 'heartbeat'):
        fields = {'peer_count', 'bad_sync_timeout', 'outlier_percent'} if kind == 'stats' else {'server_time'}
        if type(value) is not dict or value.keys() != fields:
            raise ValueError('client metadata fields')
        value = {k: scalar(value[k]) for k in fields}
    else:
        raise ValueError('unsupported client output')
    return {'kind': kind, 'value': value}


def validate_output(payload):
    if type(payload) is not dict or payload.keys() != {'kind', 'value'}:
        raise ValueError('output envelope')
    return output({payload['kind']: payload['value']})
