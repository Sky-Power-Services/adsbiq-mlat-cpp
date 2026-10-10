# SPDX-License-Identifier: AGPL-3.0-or-later
"""Stream privacy-filtered source status rows, never copy whole snapshots."""
from .schema import number, hexstr
try:
    from _mlat_capture_native import aircraft_row as _native_aircraft
except ImportError:
    _native_aircraft = None
try:
    from _mlat_capture_native import client_row as _native_client, sync_row as _native_sync
except ImportError:
    _native_client = _native_sync = None

AIR_REQUIRED = frozenset(('icao', 'elapsed_seen', 'interesting', 'allow_mlat', 'tracking',
    'sync_interest', 'mlat_interest', 'adsb_seen', 'mlat_message_count', 'mlat_result_count',
    'mlat_kalman_count', 'sync_count_1min', 'sync_bad_percent'))
AIR_OPTIONAL = frozenset(('last_result', 'lat', 'lon', 'alt', 'heading', 'speed', 'tracking_receivers'))
CLIENT_NUMBERS = ('lat', 'lon', 'alt', 'message_rate', 'peer_count', 'bad_sync_timeout', 'outlier_percent')
CLIENT_FIELDS = frozenset((*CLIENT_NUMBERS, 'privacy', 'sync_interest', 'mlat_interest'))
TABLE_LIMITS = {'aircraft': 100000, 'clients': 4096, 'sync': 4096}


def counter(value, maximum=65536):
    if type(value) is not int or not 1 <= value <= maximum:
        raise ValueError('status identity bound')
    return value


def aircraft(value, map_uid):
    if _native_aircraft is not None:
        return _native_aircraft(value, map_uid)
    return _python_aircraft(value, map_uid)


def _python_aircraft(value, map_uid):
    if type(value) is not dict or not AIR_REQUIRED <= value.keys() <= AIR_REQUIRED | AIR_OPTIONAL:
        raise ValueError('aircraft status fields')
    row = {k: number(v) for k, v in value.items() if k not in ('icao', 'tracking_receivers')}
    row['icao'] = hexstr(value['icao'], (6,))
    if 'tracking_receivers' in value:
        uids = value['tracking_receivers']
        if type(uids) not in (tuple, list) or len(uids) > 4096:
            raise ValueError('tracking list bound')
        row['tracking_receivers'] = tuple(counter(map_uid(uid)) for uid in uids)
    return row


def client(value):
    if _native_client is not None: return _native_client(value)
    return _python_client(value)


def _python_client(value):
    # Select only numeric/aircraft fields, never inspect user/UUID/IP/port,
    # connection strings, text coordinates or repr(bad_peer_list).
    row = {k: number(value[k]) for k in CLIENT_NUMBERS}
    if type(value['privacy']) is not bool:
        raise ValueError('privacy type')
    row['privacy'] = value['privacy']
    for key in ('sync_interest', 'mlat_interest'):
        values = value[key]
        if type(values) not in (tuple, list) or len(values) > 4096:
            raise ValueError('status interest bound')
        row[key] = tuple(hexstr(x, (6,)) for x in values)
    return row


def sync(value, map_user):
    if _native_sync is not None: return _native_sync(value, map_user)
    return _python_sync(value, map_user)


def _python_sync(value, map_user):
    if type(value) is not dict or value.keys() != {'peers', 'bad_syncs', 'lat', 'lon'}:
        raise ValueError('sync fields')
    peers = value['peers']
    if type(peers) is not dict or len(peers) > 4096:
        raise ValueError('sync peer bound')
    rows = []
    for user, state in peers.items():
        if type(state) not in (tuple, list) or len(state) != 8:
            raise ValueError('clock pair state shape')
        numbers = tuple(int(x) if type(x) is bool and i == 4 else number(x) for i, x in enumerate(state))
        rows.append((counter(map_user(user)), numbers))
    return dict(peers=tuple(rows), bad_syncs=number(value['bad_syncs']),
        lat=None if value['lat'] is None else number(value['lat']),
        lon=None if value['lon'] is None else number(value['lon']))


def validate_row(table, key, value, active):
    def identity(i):
        if i not in active:
            raise ValueError('status references inactive receiver')
        return counter(i)
    if table == 'aircraft':
        hexstr(key, (6,))
        aircraft(value, identity)
        if key != value['icao']:
            raise ValueError('aircraft key mismatch')
    elif table == 'clients':
        identity(key)
        if type(value) is not dict or value.keys() != CLIENT_FIELDS:
            raise ValueError('private or unknown client status fields')
        client(value)
    elif table == 'sync':
        identity(key)
        if type(value) is not dict or value.keys() != {'peers', 'bad_syncs', 'lat', 'lon'}:
            raise ValueError('sync fields')
        if type(value['peers']) not in (tuple, list) or len(value['peers']) > 4096:
            raise ValueError('peer list bound')
        peers = {}
        for peer, state in value['peers']:
            identity(peer)
            if peer in peers:
                raise ValueError('duplicate peer')
            peers[peer] = state
        sync(dict(value, peers=peers), identity)
    else:
        raise ValueError('status table')


class StatusValidator:
    """One snapshot at a time; bounded key sets, no retained values/geometry."""
    def __init__(self):
        self.sequence = 0
        self.counts = None
        self.keys = {}

    def event(self, kind, payload, active):
        if kind == 'status_begin':
            if self.counts is not None or type(payload) is not dict or payload.keys() != {'id', 'wall', 'counts'}:
                raise ValueError('status begin')
            if type(payload['id']) is not int or payload['id'] != self.sequence + 1:
                raise ValueError('status sequence')
            number(payload['wall'])
            counts = payload['counts']
            if type(counts) is not dict or counts.keys() != TABLE_LIMITS.keys():
                raise ValueError('status counts')
            if any(type(counts[k]) is not int or not 0 <= counts[k] <= limit for k, limit in TABLE_LIMITS.items()):
                raise ValueError('status count bounds')
            self.sequence = payload['id']
            self.counts = counts
            self.keys = {k: set() for k in TABLE_LIMITS}
        elif kind == 'status_row':
            if self.counts is None or type(payload) not in (list, tuple) or len(payload) != 4:
                raise ValueError('status row without begin')
            sequence, table, key, value = payload
            if type(sequence) is not int or sequence != self.sequence or table not in self.keys:
                raise ValueError('status row sequence')
            validate_row(table, key, value, active)
            if key in self.keys[table] or len(self.keys[table]) >= self.counts[table]:
                raise ValueError('duplicate or excess status row')
            self.keys[table].add(key)
        elif kind == 'status_end':
            if self.counts is None or type(payload) is not int or payload != self.sequence:
                raise ValueError('status end sequence')
            if any(len(self.keys[k]) != n for k, n in self.counts.items()):
                raise ValueError('missing status rows')
            self.counts = None
            self.keys.clear()
        else:
            raise ValueError('unknown status event')
