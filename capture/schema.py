# SPDX-License-Identifier: AGPL-3.0-or-later
"""Allowlist-only ingress capture. No credentials/identity/free-text persistence."""
import math
import re

HEX = re.compile(r'^[0-9a-fA-F]+$')
CLOCKS = {'radarcape_gps', 'radarcape', 'beast', 'radarcape_12mhz',
          'sbs', 'dump1090', 'unknown'}
COMPRESSIONS = {'zlib2', 'zlib', 'none'}
ORDER = ('sync', 'mlat', 'seen', 'lost', 'input_connected', 'input_disconnected',
         'clock_reset', 'clock_jump', 'heartbeat', 'rate_report', 'quine')


def number(value):
    if type(value) not in (int, float) or not math.isfinite(value) or abs(value) > 1e18:
        raise ValueError('invalid number')
    return value


def hexstr(value, lengths):
    if type(value) is not str or len(value) not in lengths or not HEX.fullmatch(value):
        raise ValueError('invalid hex')
    return value


def ingress(message):
    """Return (key, immutable sanitized payload) matching first-key dispatch.

    Raising invalidates capture, not the original message/server. Unknown keys
    and quine requests are deliberately excluded from parity capture.
    """
    if type(message) is not dict:
        raise ValueError('not object')
    kind = next((key for key in ORDER if key in message), None)
    value = message.get(kind)
    if kind == 'sync':
        return kind, (number(value['et']), number(value['ot']),
                      hexstr(value['em'], (28,)), hexstr(value['om'], (28,)))
    if kind == 'mlat':
        return kind, (number(value['t']), hexstr(value['m'], (14, 28)))
    if kind in ('seen', 'lost'):
        if type(value) is not list or len(value) > 4096:
            raise ValueError('aircraft list bound')
        return kind, tuple(hexstr(x, range(1, 7)) for x in value)
    if kind == 'rate_report':
        if type(value) is not dict or len(value) > 2048:
            raise ValueError('rate report bound')
        pairs = []
        for icao, rate in value.items():
            address = hexstr(icao, range(1, 7))
            pairs.append((address, number(rate)))
        return kind, tuple(pairs)
    if kind in ('input_connected', 'input_disconnected', 'clock_reset', 'clock_jump', 'heartbeat'):
        # Payload ignored by baseline; may contain untrusted free text.
        return kind, None
    raise ValueError('unsupported input')


def connection(receiver, client):
    lat, lon, alt = receiver.position_llh
    lat, lon, alt = number(lat), number(lon), number(alt)
    if not -90 <= lat <= 90 or not -180 <= lon <= 360 or not -1000 <= alt <= 10000:
        raise ValueError('geometry bounds')
    clock = client.capture_clock_type
    if clock not in CLOCKS or client.compress not in COMPRESSIONS:
        raise ValueError('negotiation bounds')
    # No user, UUID, auth, source IP/port, client_version or MOTD. Receiver UID
    # is an opaque process-local counter, not a hash of identifying material.
    return (lat, lon, alt, bool(receiver.privacy), clock, client.compress,
            bool(client.use_return_results), bool(client.return_stats))
