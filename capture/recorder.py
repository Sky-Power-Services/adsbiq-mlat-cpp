# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fail-open recorder. No network access; caller supplies sanitized immutable data.

Not installed in production. Payload schema/sanitization lives in schema.py.
The producer never waits for disk or for a queue lock. Queue-full/lock-contention
ends capture, not feeder service. A recording is usable only with a completed,
loss-free manifest. An absent manifest (crash/blocked disk) is invalid.
"""
from collections import deque
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import threading
import time
from .codec import encode
from .blocks import encode_block, FORMAT as COMPRESSED_FORMAT, LZ4_FORMAT, MAX_TOTAL_PLAIN
from .schema import ingress


class _PackedBatch:
    """Owned native bytes; materialize individual records only at chunk/cap edge."""
    __slots__ = ('data', 'sizes')

    def __init__(self, data, sizes):
        self.data, self.sizes = data, sizes

    def __len__(self):
        return len(self.sizes)//4

    def __iter__(self):
        offset = 0
        for (size,) in struct.iter_unpack('<I', self.sizes):
            yield self.data[offset:offset+size]
            offset += size

    def clear(self):
        self.data = self.sizes = b''


class Recorder:
    def __init__(self, directory, *, duration=120, queue_bytes=8*1024*1024,
                 chunk_bytes=64*1024*1024, total_bytes=512*1024*1024,
                 min_free_bytes=1024*1024*1024, max_record_bytes=65536, native=False, compress=False, event_clock=None, lz4_acceleration=1):
        if event_clock is not None and (native or not callable(event_clock)):
            raise ValueError("offline event clock requires callable and Python encoder")
        # Offline synthetic time affects envelopes only. Disk deadlines and
        # writer scheduling remain on real time; native timestamps cannot mix.
        self.event_clock = event_clock
        self._last_event_elapsed = -1
        if not math.isfinite(duration) or not 0 < duration <= 3600:
            raise ValueError('duration must be finite and <=3600 seconds')
        if not 256 <= max_record_bytes <= 65536:
            raise ValueError('record limit out of bounds')
        if not max_record_bytes <= queue_bytes <= 8*1024*1024:
            raise ValueError('queue limit out of bounds')
        if not max_record_bytes <= chunk_bytes <= 64*1024*1024:
            raise ValueError('chunk limit out of bounds')
        if not chunk_bytes <= total_bytes <= 512*1024*1024:
            raise ValueError('total limit out of bounds')
        if min_free_bytes < 0:
            raise ValueError('free disk floor must be nonnegative')
        if type(compress) not in (bool, str) or compress not in (False, True, 'lz4'):
            raise ValueError("compression option must be boolean or lz4")
        self.compress = bool(compress)
        if (type(lz4_acceleration) is not int or not 1 <= lz4_acceleration <= 16
                or (compress != 'lz4' and lz4_acceleration != 1)):
            raise ValueError('LZ4 acceleration requires LZ4 and integer1..16')
        self.lz4_acceleration = lz4_acceleration
        self.compression_codec = 'lz4' if compress == 'lz4' else 'zlib'
        if compress == 'lz4':
            from .lz4_codec import library
            library()  # Fail before creating files/starting writer if unavailable.
        self.logical_bytes_written = 0
        self._native_encode = None
        self._native_aircraft = None
        self._native_record = None
        self._native_aircraft_batch = None
        self._native_random = None
        self._native_queue = None
        if native:
            from _mlat_capture_native import encode_input, encode_aircraft, record_input, record_aircraft_batch, encode_random_choice, Queue
            self._native_encode = encode_input
            self._native_aircraft = encode_aircraft
            self._native_record = record_input
            self._native_aircraft_batch = record_aircraft_batch
            self._native_random = encode_random_choice
            self._native_queue = Queue(queue_bytes, max_record_bytes)
        self.directory = Path(directory)
        # Require a new directory; never append to or overwrite earlier evidence.
        self.directory.mkdir(mode=0o700, parents=False, exist_ok=False)
        if os.name != 'nt':
            os.chmod(self.directory, 0o700)
        self.duration = duration
        self.queue_limit = queue_bytes
        self.chunk_limit = chunk_bytes
        self.total_limit = total_bytes
        self.min_free = min_free_bytes
        self.record_limit = max_record_bytes
        self.started_mono = time.monotonic_ns()
        self.started_utc = time.time_ns()
        self.deadline = time.monotonic() + duration
        self.active = True
        self.reason = None
        self.errors = 0
        self.dropped = 0
        self.submitted = 0
        self.written = 0
        self.queue_peak = 0
        self.queued_bytes = 0
        self.bytes_written = 0
        self._queue = deque()
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._done = threading.Event()
        self._thread = threading.Thread(target=self._run, name='mlat-capture-writer', daemon=True)
        self._thread.start()

    @property
    def submitted(self):
        return self._native_queue.submitted if self._native_queue is not None else self._submitted

    @submitted.setter
    def submitted(self, value):
        self._submitted = value

    @property
    def queued_bytes(self):
        return self._native_queue.queued_bytes if self._native_queue is not None else self._queued_bytes

    @queued_bytes.setter
    def queued_bytes(self, value):
        self._queued_bytes = value

    @property
    def queue_peak(self):
        return self._native_queue.peak if self._native_queue is not None else self._queue_peak

    @queue_peak.setter
    def queue_peak(self, value):
        self._queue_peak = value

    def stop(self, reason='requested'):
        if self._native_queue is not None:
            self._native_queue.stop()
        self.active = False
        if self.reason is None:
            self.reason = reason
        self._wake.set()

    def invalidate(self, reason):
        """Use fixed error codes only; never persist exception text or payloads."""
        if not self.errors:
            self.reason = reason
        self.errors += 1
        self.stop(reason)

    def submit(self, kind, receiver, payload):
        """Single event-loop producer; immutable sanitized payload only.

        Binary encoding bounds the queued payload memory but costs CPU. The
        off-box load gate must pass before this can be used with live feeders.
        """
        if not self.active:
            return False
        try:
            if time.monotonic() >= self.deadline:
                self.stop('duration')
                return False
            # Inputs are schema-bounded BEFORE serialization (schema.py).
            if self.event_clock is None:
                elapsed, utc = time.monotonic_ns() - self.started_mono, time.time_ns()
            else:
                elapsed, utc = self.event_clock()
                if (type(elapsed) is not int or type(utc) is not int or elapsed < self._last_event_elapsed
                        or not 0 <= elapsed < 2**64 or not 0 < utc < 2**64):
                    raise ValueError('offline event clock bounds')
                self._last_event_elapsed = elapsed
            data = encode(self.submitted, elapsed, utc, kind, receiver, payload)
            return self._enqueue(data)
        except Exception:
            self.dropped += 1
            self.invalidate('producer_error')
            return False

    def _enqueue(self, data):
        if self._native_queue is not None:
            status = self._native_queue.put(data)
            if status == 1:
                return True
            if status:
                self.dropped += 1
                self.invalidate('record_limit' if status == 3 else 'queue_limit')
            return False
        size = len(data)
        if size > self.record_limit:
            self.dropped += 1
            self.invalidate('record_limit')
            return False
        if not self._lock.acquire(False):
            self.dropped += 1
            self.invalidate('queue_contention')
            return False
        try:
            if not self.active:
                return False
            queued = self.queued_bytes + size
            if queued > self.queue_limit:
                self.dropped += 1
                self.invalidate('queue_limit')
                return False
            self._queue.append(data)
            self.queued_bytes = queued
            if queued > self.queue_peak:
                self.queue_peak = queued
            self.submitted += 1
        finally:
            self._lock.release()
        # Writer polls every 200 ms while idle; no per-message wake syscall.
        # The unchanged byte cap bounds accumulation; stop wakes immediately.
        return True

    def record_random_choice(self, stream, op, population, selected):
        if not self.active: return False
        try:
            if self._native_random is not None:
                data = self._native_random(self.submitted, self.started_mono, stream, op,
                                           population, selected, self.deadline)
                if data is False:
                    self.stop('duration'); return False
                return self._enqueue(data)
            return self.submit('random_choice', 0, dict(stream=stream, op=op,
                population=population, selected=selected))
        except Exception:
            self.dropped += 1
            self.invalidate('random_schema')
            return False

    def record_aircraft_batch(self, snapshot, rows, uid_map):
        if not self.active: return False
        try:
            status = self._native_aircraft_batch(self._native_queue, self.started_mono,
                                                 self.deadline, snapshot, rows, uid_map)
            if status == 1: return True
            if status == 4: self.stop('duration')
            elif status in (2, 3):
                self.dropped += 1
                self.invalidate('record_limit' if status == 3 else 'queue_limit')
            return False
        except Exception:
            self.dropped += 1
            self.invalidate('status_schema')
            return False

    def record_aircraft(self, snapshot, key, row, map_uid):
        if not self.active: return False
        try:
            if self._native_aircraft is not None:
                data = self._native_aircraft(self.submitted, self.started_mono,
                    snapshot, key, row, map_uid, self.deadline)
                if data is False:
                    self.stop('duration'); return False
                return self._enqueue(data)
            from .status import aircraft
            return self.submit('status_row', 0, (snapshot, 'aircraft', key, aircraft(row, map_uid)))
        except Exception:
            self.dropped += 1
            self.invalidate('status_schema')
            return False

    def record_input(self, receiver, message):
        """Fail-open integration boundary for an already parsed inbound message.

        Call alongside (not instead of) normal dispatch. No schema exception
        may escape into the feeder connection. Disabled capture does no parsing.
        """
        if not self.active:
            return False
        try:
            if self._native_encode is not None:
                data = self._native_encode(self._native_queue.submitted, self.started_mono, receiver, message, self.deadline)
                if data is False:
                    self.stop('duration')
                    return False
                if data is not None:
                    return self._enqueue(data)
            kind, payload = ingress(message)
            return self.submit(kind, receiver, payload)
        except Exception:
            self.dropped += 1
            self.invalidate('input_schema')
            return False

    def close(self, timeout=2):
        """Only shutdown/test caller may wait; never call this on packet ingress."""
        self.stop()
        return self._done.wait(timeout)

    def _take_batch(self):
        if self._native_queue is not None:
            return _PackedBatch(*self._native_queue.take_packed())
        with self._lock:
            batch = self._queue
            self._queue = deque()
            self.queued_bytes = 0
        return batch

    def _open_chunk(self, index):
        path = self.directory / f'events-{index:04d}.mlatcap'
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        return os.fdopen(fd, 'wb', buffering=65536), path.name

    def _write_manifest(self, chunks):
        manifest = {'event_clock_domain': 'system' if self.event_clock is None else 'offline-injected', 'schema': 2, 'format': (LZ4_FORMAT if self.compression_codec == 'lz4' else COMPRESSED_FORMAT) if self.compress else 'mlatcap-v1', 'started_utc_ns': self.started_utc,
                    'ended_utc_ns': time.time_ns(), 'reason': self.reason,
                    'complete': True, 'loss_free': self.errors == 0 and self.dropped == 0
                    and self.submitted == self.written,
                    'submitted': self.submitted, 'written': self.written,
                    'dropped': self.dropped, 'errors': self.errors,
                    'queue_peak_bytes': self.queue_peak, 'bytes_written': self.bytes_written,
                    'logical_bytes_written': self.logical_bytes_written,
                    'limits': {'duration_s': self.duration, 'queue_bytes': self.queue_limit,
                               'chunk_bytes': self.chunk_limit, 'total_bytes': self.total_limit,
                               'logical_bytes': MAX_TOTAL_PLAIN},
                    'chunks': chunks}
        if self.compress and self.compression_codec == 'lz4':
            manifest['lz4_acceleration'] = self.lz4_acceleration
        path = self.directory / 'manifest.json'
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(manifest, f, indent=2)
            f.write('\n')
            f.flush()
            os.fsync(f.fileno())

    def _run(self):
        f = None
        chunks = []
        digest = None
        chunk_size = 0
        chunk_logical_size = 0
        name = None
        next_disk_check = 0

        def finish_chunk():
            nonlocal f
            if f is not None:
                f.flush()
                os.fsync(f.fileno())
                f.close()
                f = None
                chunks.append({'file': name, 'bytes': chunk_size, 'logical_bytes': chunk_logical_size, 'sha256': digest.hexdigest()})

        try:
            while self.active or self.queued_bytes:
                if time.monotonic() >= self.deadline:
                    self.stop('duration')
                batch = self._take_batch()
                # Common bounded writer batches fit one chunk. Aggregate at most the
                # bounded queue's 8 MiB, then perform one write/hash operation.
                # Boundary batches retain the per-record path below so caps
                # never split a record or discard a valid prefix unnecessarily.
                if batch:
                    combined = batch.data if isinstance(batch, _PackedBatch) else b''.join(batch)
                    logical_size = len(combined)
                    if self.compress:
                        combined = encode_block(combined, self.compression_codec, self.lz4_acceleration)
                    size = len(combined)
                    if (size + chunk_size <= self.chunk_limit and
                            logical_size + chunk_logical_size <= self.chunk_limit and
                            logical_size + self.logical_bytes_written <= MAX_TOTAL_PLAIN and
                            size + self.bytes_written <= self.total_limit):
                        if time.monotonic() >= next_disk_check:
                            if shutil.disk_usage(self.directory).free < self.min_free:
                                self.invalidate('disk_floor')
                                raise OSError('capture disk floor')
                            next_disk_check = time.monotonic() + 1
                        if f is None:
                            f, name = self._open_chunk(len(chunks))
                            digest = hashlib.sha256()
                            chunk_size = 0
                            chunk_logical_size = 0
                        if f.write(combined) != size:
                            raise OSError('capture short write')
                        digest.update(combined)
                        chunk_size += size
                        chunk_logical_size += logical_size
                        self.bytes_written += size
                        self.logical_bytes_written += logical_size
                        self.written += len(batch)
                        batch.clear()
                    del combined
                # Packed native batches materialize their bounded per-record
                # slices only here; common writes never allocate them.
                if batch and not isinstance(batch, deque):
                    batch = deque(batch)
                while batch:
                    data = batch.popleft()
                    logical_size = len(data)
                    if self.compress:
                        data = encode_block(data, self.compression_codec, self.lz4_acceleration)
                    if len(data) > self.chunk_limit:
                        self.dropped += 1 + len(batch)
                        self.invalidate('chunk_limit')
                        batch.clear()
                        break
                    if self.bytes_written + len(data) > self.total_limit or self.logical_bytes_written + logical_size > MAX_TOTAL_PLAIN:
                        self.dropped += 1 + len(batch)
                        self.invalidate('total_limit')
                        batch.clear()
                        break
                    if time.monotonic() >= next_disk_check:
                        if shutil.disk_usage(self.directory).free < self.min_free:
                            self.dropped += 1 + len(batch)
                            self.invalidate('disk_floor')
                            batch.clear()
                            break
                        next_disk_check = time.monotonic() + 1
                    if f is not None and (chunk_size + len(data) > self.chunk_limit or chunk_logical_size + logical_size > self.chunk_limit):
                        finish_chunk()
                    if f is None:
                        f, name = self._open_chunk(len(chunks))
                        digest = hashlib.sha256()
                        chunk_size = 0
                        chunk_logical_size = 0
                    if f.write(data) != len(data):
                        raise OSError('capture short write')
                    digest.update(data)
                    chunk_size += len(data)
                    chunk_logical_size += logical_size
                    self.bytes_written += len(data)
                    self.logical_bytes_written += logical_size
                    self.written += 1
                if self.errors:
                    remaining = self._take_batch()
                    self.dropped += len(remaining)
                    break
                if not self.queued_bytes:
                    # Amortize writer wakeups/copies; producers remain entirely
                    # nonblocking and the 8MiB queue cap is unchanged. stop()
                    # signals this event, so shutdown never waits for the poll.
                    self._wake.wait(min(0.5, max(0., self.deadline-time.monotonic())))
                    self._wake.clear()
            finish_chunk()
        except Exception:
            self.invalidate('writer_error')
            self.dropped = max(self.dropped, self.submitted - self.written)
        finally:
            if f is not None:
                try:
                    f.close()
                except Exception:
                    self.errors += 1
            try:
                self._write_manifest(chunks)
            except Exception:
                # Missing/partial manifest is never usable for replay.
                self.errors += 1
            self.active = False
            self._done.set()
