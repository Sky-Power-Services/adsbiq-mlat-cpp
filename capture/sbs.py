# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded exact SBS text validation; no receiver/destination identity fields."""
import re
from .schema import number, hexstr


def line(value):
    if type(value) is not str or len(value) > 2048 or not value.isascii():
        raise ValueError('SBS line bound')
    if value == '\n':
        return value
    if not value.endswith('\n') or '\n' in value[:-1] or '\r' in value:
        raise ValueError('SBS line framing')
    fields = value[:-1].split(',')
    if len(fields) != 22 or fields[:4] != ['MSG', '3', '1', '1'] or fields[5] != '1':
        raise ValueError('SBS schema')
    hexstr(fields[4], (6,))
    for i in (6, 8):
        if not re.fullmatch(r'[0-9]{4}/[0-9]{2}/[0-9]{2}', fields[i]):
            raise ValueError('SBS date format')
    for i in (7, 9):
        # Preserve pinned formatter's .1000 rounding quirk (no carry).
        if not re.fullmatch(r'[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3,4}', fields[i]):
            raise ValueError('SBS time format')
    if not re.fullmatch(r'[A-Z0-9 _#?]{0,8}', fields[10]) or not re.fullmatch(r'(?:[0-7]{4})?', fields[17]):
        raise ValueError('SBS aircraft text')
    for i in (11, 12, 13, 14, 15, 16, 18, 19):
        if fields[i]:
            if not re.fullmatch(r'-?[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?', fields[i]):
                raise ValueError('SBS numeric field')
            number(float(fields[i]))
    if fields[20] or fields[21]:
        raise ValueError('SBS unused fields')
    return value


class Validator:
    def __init__(self):
        self.sequence = 0
        self.active = {}

    def event(self, kind, payload):
        if kind == 'sbs_connect':
            if type(payload) not in (list, tuple) or len(payload) != 2:
                raise ValueError('SBS connection schema')
            identity, filtered = payload
            if (type(identity) is not int or identity != self.sequence + 1 or identity > 65536
                    or type(filtered) is not bool or len(self.active) >= 1024):
                raise ValueError('SBS connection bounds')
            self.sequence = identity
            self.active[identity] = filtered
        elif kind == 'sbs_output':
            if type(payload) not in (list, tuple) or len(payload) != 2:
                raise ValueError('SBS output schema')
            identity, text = payload
            if type(identity) is not int or identity not in self.active:
                raise ValueError('inactive SBS sink')
            line(text)
        elif kind == 'sbs_disconnect':
            if type(payload) is not int or payload not in self.active:
                raise ValueError('inactive SBS sink')
            del self.active[payload]
        else:
            raise ValueError('unknown SBS event')
