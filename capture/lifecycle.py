# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded, fail-open receiver-generation capture; not installed in production.

Identity strings exist only as keys in bounded in-process lookup tables. Only
independently allocated counters leave this module; no hashes or identifiers
are persisted. Call on accepted handshake completion, before its first input.
"""
import weakref
from .schema import CLOCKS, COMPRESSIONS, number

FIELDS = frozenset(('llh', 'privacy', 'clock', 'compression', 'result_format',
                    'return_stats', 'transport', 'user_group', 'uuid_group', 'source_uid'))
MAX_GROUPS = 8192
MAX_GENERATIONS = 65536


def validate_connection(payload):
    if type(payload) is not dict or payload.keys() != FIELDS:
        raise ValueError('connection fields')
    llh = payload['llh']
    if type(llh) not in (list, tuple) or len(llh) != 3:
        raise ValueError('geometry shape')
    lat, lon, alt = map(number, llh)
    if not -90 <= lat <= 90 or not -180 <= lon <= 360 or not -1000 <= alt <= 10000:
        raise ValueError('geometry bounds')
    if (payload['clock'] not in CLOCKS or payload['compression'] not in COMPRESSIONS
            or payload['result_format'] not in ('none', 'old', 'ecef')
            or payload['transport'] not in ('tcp', 'udp')
            or any(type(payload[k]) is not bool for k in ('privacy', 'return_stats'))):
        raise ValueError('negotiation bounds')
    for key, low, high in (('user_group', 1, MAX_GROUPS), ('uuid_group', 0, MAX_GROUPS),
                           ('source_uid', 0, 2**63-1)):
        if type(payload[key]) is not int or not low <= payload[key] <= high:
            raise ValueError('identity counter bounds')
    return payload


def negotiated_connection(receiver, client, clock_type, user_group, uuid_group):
    # The baseline stores result format by selecting a bound method, not an
    # attribute. Never guess from a method name or reread the raw handshake.
    method = client.report_mlat_position
    if not client.use_return_results:
        if method != client.report_mlat_position_discard:
            raise ValueError('unexpected discarded result handler')
        result_format = 'none'
    elif method == client.report_mlat_position_old:
        result_format = 'old'
    elif method == client.report_mlat_position_ecef:
        result_format = 'ecef'
    else:
        raise ValueError('unexpected result handler')
    return validate_connection(dict(llh=tuple(receiver.position_llh),
        privacy=bool(receiver.privacy), clock=clock_type, compression=client.compress,
        result_format=result_format, return_stats=bool(client.return_stats),
        transport='udp' if client.use_udp else 'tcp', user_group=user_group,
        uuid_group=uuid_group, source_uid=receiver.uid))


class Lifecycle:
    def __init__(self, recorder, *, max_active=4096):
        if type(max_active) is not int or not 1 <= max_active <= 4096:
            raise ValueError('active receiver bound')
        self.recorder = recorder
        self.max_active = max_active
        self.active = {}
        self.history = {}
        self.users = {}
        self.uuids = {}
        self.generation = 0

    def _group(self, table, key):
        if type(key) is not str or not 0 < len(key) <= 256:
            raise ValueError('identity shape')
        group = table.get(key)
        if group is None:
            if len(table) >= MAX_GROUPS:
                raise ValueError('identity table bound')
            group = len(table) + 1
            table[key] = group
        return group

    def _fail(self):
        self.recorder.invalidate('lifecycle_schema')
        self.clear()

    def clear(self):
        self.active.clear()
        self.history.clear()
        self.users.clear()
        self.uuids.clear()

    def connect(self, receiver, client, clock_type):
        if not self.recorder.active:
            self.clear()
            return None
        try:
            if (id(receiver) in self.history or len(self.active) >= self.max_active
                    or self.generation >= MAX_GENERATIONS):
                raise ValueError('receiver lifecycle bound')
            user_group = self._group(self.users, receiver.user)
            uuid_group = self._group(self.uuids, receiver.uuid) if receiver.uuid else 0
            payload = negotiated_connection(receiver, client, clock_type, user_group, uuid_group)
            generation = self.generation + 1
            key = id(receiver)
            def collected(ref, key=key, history=self.history):
                if history.get(key, (None,))[0] is ref:
                    del history[key]
            reference = weakref.ref(receiver, collected)
            if not self.recorder.submit('connect_v2', generation, payload):
                self.clear()
                return None
            # Retain the actual active object to prevent Python id reuse. Drop
            # it immediately on disconnect; generations are never recycled.
            self.active[id(receiver)] = (receiver, generation)
            self.history[id(receiver)] = (reference, generation)
            self.generation = generation
            return generation
        except Exception:
            self._fail()
            return None

    def identity(self, receiver):
        entry = self.active.get(id(receiver))
        if entry is None or entry[0] is not receiver:
            raise ValueError('inactive receiver')
        return entry[1]

    def known_identity(self, receiver):
        entry = self.history.get(id(receiver))
        if entry is None or entry[0]() is not receiver:
            raise ValueError("unknown receiver generation")
        return entry[1]

    def disconnect(self, receiver):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            generation = self.identity(receiver)
            del self.active[id(receiver)]
            return self.recorder.submit('disconnect', generation, None)
        except Exception:
            self._fail()
            return False

    def input(self, receiver, message):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            return self.recorder.record_input(self.identity(receiver), message)
        except Exception:
            self._fail()
            return False
