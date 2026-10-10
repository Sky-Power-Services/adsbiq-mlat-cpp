# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read-only, streaming capture validation; never prints data or geometry."""
import hashlib
import json
from pathlib import Path
import re

from .codec import records
from .blocks import BlockReader, FORMAT as COMPRESSED_FORMAT, LZ4_FORMAT, MAX_CHUNK_PLAIN, MAX_TOTAL_PLAIN
from .schema import ingress
from .reference import validate_output
from .randomness import validate as validate_random_choice
from .solution import validate as validate_solution
from .status import StatusValidator
from .sbs import Validator as SbsValidator
from .lifecycle import validate_connection, MAX_GENERATIONS


def verify(directory, *, min_receivers=700):
    directory = Path(directory).resolve(strict=True)
    manifest_path = directory / 'manifest.json'
    if manifest_path.is_symlink() or manifest_path.stat().st_size > 65536:
        raise ValueError('unsafe manifest')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if (manifest.get('schema') != 2 or manifest.get('format') not in ('mlatcap-v1', COMPRESSED_FORMAT, LZ4_FORMAT)
            or manifest.get('complete') is not True or manifest.get('loss_free') is not True
            or manifest.get('errors') != 0 or manifest.get('dropped') != 0):
        raise ValueError('incomplete or lossy capture')
    clock_domain = manifest.get('event_clock_domain', 'unspecified')
    if clock_domain not in ('system', 'offline-injected', 'unspecified'):
        raise ValueError('capture clock domain')
    chunks = manifest.get('chunks', [])
    if not isinstance(chunks, list) or len(chunks) > 8192:
        raise ValueError('chunk count bound')
    total_bytes = 0
    declared_logical = 0
    logical_sizes = []
    seen_paths = set()
    # Preflight and hash everything BEFORE allowing any replay. Files must
    # remain private/immutable after verification (no untrusted concurrent writer).
    paths = []
    for chunk in chunks:
        name = chunk.get('file', '')
        if not re.fullmatch(r'events-[0-9]{4}\.mlatcap', name) or name in seen_paths:
            raise ValueError('invalid chunk path')
        seen_paths.add(name)
        p = directory / name
        if p.is_symlink() or not p.is_file():
            raise ValueError('unsafe chunk')
        size = p.stat().st_size
        if size != chunk.get('bytes') or size > 64*1024*1024:
            raise ValueError('chunk size bound or mismatch')
        logical_size = chunk.get('logical_bytes', size if manifest['format'] == 'mlatcap-v1' else None)
        if type(logical_size) is not int or not 0 <= logical_size <= MAX_CHUNK_PLAIN:
            raise ValueError('logical chunk size bound')
        declared_logical += logical_size
        if declared_logical > MAX_TOTAL_PLAIN:
            raise ValueError('logical capture budget exceeded')
        logical_sizes.append(logical_size)
        total_bytes += size
        if total_bytes > 512*1024*1024:
            raise ValueError('capture budget exceeded')
        digest = hashlib.sha256()
        with p.open('rb') as f:
            while data := f.read(65536):
                digest.update(data)
        if digest.hexdigest() != chunk.get('sha256'):
            raise ValueError('chunk hash mismatch')
        paths.append(p)
    if total_bytes != manifest.get('bytes_written'):
        raise ValueError('total size mismatch')
    status = StatusValidator()
    sbs = SbsValidator()
    active = set()
    active_source_uids = {}
    generation_source_uids = {}
    connected = set()
    counts = {}
    sequence = 0
    last_elapsed = -1
    peak_connected = 0
    logical_bytes = 0
    for path, expected_logical in zip(paths, logical_sizes):
        with path.open('rb') as f:
            codec = 'lz4' if manifest['format'] == LZ4_FORMAT else 'zlib'
            stream = BlockReader(f, codec=codec) if manifest['format'] != 'mlatcap-v1' else f
            for seq, elapsed, utc, kind, receiver, payload in records(stream):
                if seq != sequence or elapsed < last_elapsed:
                    raise ValueError('sequence loss or nonmonotonic arrival time')
                if utc <= 0:
                    raise ValueError('invalid UTC timestamp')
                sequence += 1
                last_elapsed = elapsed
                counts[kind] = counts.get(kind, 0) + 1
                if status.counts is not None and kind not in ('status_row', 'status_end'):
                    raise ValueError('interleaved or incomplete status snapshot')
                if kind == 'connect_v2':
                    validate_connection(payload)
                    uid = payload['source_uid']
                    if receiver == 0 or receiver in connected or len(connected) >= MAX_GENERATIONS or len(active) >= 4096 or uid in active_source_uids:
                        raise ValueError('invalid receiver generation or active UID reuse')
                    connected.add(receiver)
                    active.add(receiver)
                    active_source_uids[uid] = receiver
                    generation_source_uids[receiver] = uid
                    peak_connected = max(peak_connected, len(active))
                elif kind == 'connect':
                    if receiver in connected or len(connected) >= MAX_GENERATIONS or len(active) >= 4096 or type(payload) is not list or len(payload) != 8:
                        raise ValueError('invalid or duplicate receiver initialization')
                    # Avoid printing or returning these restricted coordinates.
                    lat, lon, alt, privacy, clock, compression, returns, stats = payload
                    from .schema import CLOCKS, COMPRESSIONS, number
                    number(lat), number(lon), number(alt)
                    if (not -90 <= lat <= 90 or not -180 <= lon <= 360
                            or not -1000 <= alt <= 10000 or clock not in CLOCKS
                            or compression not in COMPRESSIONS
                            or any(type(x) is not bool for x in (privacy, returns, stats))):
                        raise ValueError('invalid receiver initialization')
                    connected.add(receiver)
                    active.add(receiver)
                    peak_connected = max(peak_connected, len(active))
                elif kind == 'disconnect':
                    if receiver not in active:
                        raise ValueError('disconnect without active receiver')
                    if payload is not None:
                        raise ValueError('invalid disconnect payload')
                    active.remove(receiver)
                    if receiver in generation_source_uids:
                        del active_source_uids[generation_source_uids.pop(receiver)]
                elif kind in ('status_begin', 'status_row', 'status_end'):
                    if receiver != 0:
                        raise ValueError('status event receiver must be zero')
                    status.event(kind, payload, active)
                elif kind in ('sbs_connect', 'sbs_output', 'sbs_disconnect'):
                    if receiver != 0:
                        raise ValueError('SBS receiver must be zero')
                    sbs.event(kind, payload)
                elif kind == 'location':
                    from .schema import number
                    if receiver not in active or type(payload) is not list or len(payload) != 3:
                        raise ValueError('location without active receiver or geometry')
                    lat, lon, alt = map(number, payload)
                    if not -90 <= lat <= 90 or not -180 <= lon <= 360 or not -1000 <= alt <= 10000:
                        raise ValueError('location geometry bound')
                elif kind == 'loop_turn':
                    if receiver != 0 or payload not in ('start', 'pump'):
                        raise ValueError('loop turn marker')
                elif kind == 'random_choice':
                    if receiver != 0:
                        raise ValueError('choice receiver must be zero')
                    validate_random_choice(payload, connected)
                elif kind == 'solution':
                    if receiver != 0:
                        raise ValueError('solution receiver must be zero')
                    validate_solution(payload, connected)
                elif kind == 'client_output':
                    if receiver not in active:
                        raise ValueError('output without active receiver')
                    validate_output(payload)
                elif kind in ('sync', 'mlat', 'seen', 'lost', 'rate_report', 'heartbeat',
                              'input_connected', 'input_disconnected', 'clock_reset', 'clock_jump'):
                    if receiver not in active:
                        raise ValueError('input without receiver initialization')
                    if kind == 'sync':
                        ingress({'sync': dict(zip(('et', 'ot', 'em', 'om'), payload))})
                    elif kind == 'mlat':
                        ingress({'mlat': dict(zip(('t', 'm'), payload))})
                    elif kind == 'rate_report':
                        ingress({'rate_report': dict(payload)})
                    else:
                        ingress({kind: payload})
                else:
                    # Output/status schemas and adapters must be implemented
                    # before an artifact with those events can pass this gate.
                    raise ValueError('unsupported reference event schema')
            actual_logical = stream.total_plain if isinstance(stream, BlockReader) else f.tell()
            if actual_logical != expected_logical:
                raise ValueError('logical chunk size mismatch')
            logical_bytes += actual_logical
    if status.counts is not None:
        raise ValueError('unfinished status snapshot')
    if ('logical_bytes_written' in manifest or manifest['format'] != 'mlatcap-v1') and logical_bytes != manifest.get('logical_bytes_written'):
        raise ValueError('logical byte count mismatch')
    if sequence != manifest.get('written') or sequence != manifest.get('submitted'):
        raise ValueError('event count mismatch')
    if peak_connected < min_receivers:
        raise ValueError('insufficient concurrent receiver coverage')
    return {'structurally_valid': True, 'event_clock_domain': clock_domain, 'events': sequence, 'bytes': total_bytes,
            'logical_bytes': logical_bytes,
            'unique_receivers': len(connected), 'peak_connected': peak_connected,
            'event_counts': counts, 'status_snapshots': status.sequence,
            'duration_s': max(0, last_elapsed) / 1e9,
            'full_parity_ready': False,
            'limitation': 'Structural ingress check only; outputs, warm-up, timing and coverage duration unverified.'}
