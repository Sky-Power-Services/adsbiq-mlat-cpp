# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generate isolated instrumentation in memory; never edits installed sources.

Reject source drift and repeated instrumentation. The caller must explicitly
attach a Hook to coordinator._mlat_capture; absent that capture is disabled.
"""
import ast
import hashlib

JSONCLIENT_SHA256 = '61220cdaed06c662daf7860c425684e1ef606f7a1743eb1e9bcb35a8e17aecb8'
COORDINATOR_SHA256 = '16c48d38de2ce48306b9e22d2e8520b5e64a47eb4d05ba76a8d792eb830bf16c'
OUTPUT_SHA256 = '9bb594d9c2b8752424a83084e5c53263a59ea03d1b55e8465369ebf8f2628b26'
MLATTRACK_SHA256 = 'f395bd9c1766bf8c33c7cc0ec500bf83d250451c6f2f70acbd56a46d69ff229b'


def instrument_mlattrack(source):
    if type(source) is not bytes or len(source) > 1024*1024 or hashlib.sha256(source).hexdigest() != MLATTRACK_SHA256:
        raise ValueError('pinned mlattrack source mismatch')
    text = source.decode('utf-8')
    replacements = (
        ('import asyncio\n', 'import asyncio\nfrom capture.hook import emit_coordinator as _mlat_capture_emit\n'),
        ('        for handler in self.coordinator.output_handlers:\n',
         "        _mlat_capture_emit(self.coordinator, 'solution', cluster_utc, decoded.address,\n"
         "                           ecef, ecef_cov, [r for r, t, e in cluster],\n"
         "                           list(group.receivers), distinct, dof, ac.kalman, error)\n"
         '        for handler in self.coordinator.output_handlers:\n'),
    )
    for old, new in replacements:
        if text.count(old) != 1: raise ValueError('instrumentation anchor mismatch')
        text = text.replace(old, new, 1)
    ast.parse(text)
    return text


def instrument_output(source):
    if type(source) is not bytes or len(source) > 1024*1024 or hashlib.sha256(source).hexdigest() != OUTPUT_SHA256:
        raise ValueError('pinned output source mismatch')
    text = source.decode('utf-8')
    replacements = (
        ('import asyncio\n', 'import asyncio\nfrom capture.hook import emit as _mlat_capture_emit\n'),
        ('        self.logger.warning("Connection established")\n',
         '        self.logger.warning("Connection established")\n        _mlat_capture_emit(self, "sbs_connect", self, self.use_kalman_data)\n'),
        ('        self.logger.info("Connection lost")\n',
         '        self.logger.info("Connection lost")\n        _mlat_capture_emit(self, "sbs_disconnect", self)\n'),
        ("                self.writer.write(b'\\n')\n",
         "                self.writer.write(b'\\n')\n                _mlat_capture_emit(self, 'sbs_output', self, '\\n')\n"),
        ("            self.writer.write(line.encode('ascii'))\n",
         "            self.writer.write(line.encode('ascii'))\n            _mlat_capture_emit(self, 'sbs_output', self, line)\n"),
    )
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError('instrumentation anchor mismatch')
        text = text.replace(old, new, 1)
    ast.parse(text)
    return text


def instrument_coordinator(source):
    if type(source) is not bytes or len(source) > 1024*1024 or hashlib.sha256(source).hexdigest() != COORDINATOR_SHA256:
        raise ValueError('pinned coordinator source mismatch')
    text = source.decode('utf-8')
    replacements = (
        ('import asyncio\n', 'import asyncio\nfrom capture.hook import emit_coordinator as _mlat_capture_emit\n'),
        ('        self._compute_interstation_distances(receiver)\n\n    @profile.trackcpu\n    def receiver_disconnect',
         "        self._compute_interstation_distances(receiver)\n        _mlat_capture_emit(self, 'location', receiver)\n\n    @profile.trackcpu\n    def receiver_disconnect"),
        ("        syncfile = self.work_dir + '/sync.json'\n",
         "        _mlat_capture_emit(self, 'status', self, now, aircraft_state, clients, sync)\n        syncfile = self.work_dir + '/sync.json'\n"),
    )
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError('instrumentation anchor mismatch')
        text = text.replace(old, new, 1)
    ast.parse(text)
    return text


def instrument_jsonclient(source):
    if type(source) is not bytes or len(source) > 1024*1024 or hashlib.sha256(source).hexdigest() != JSONCLIENT_SHA256:
        raise ValueError('pinned jsonclient source mismatch')
    text = source.decode('utf-8')
    replacements = (
        ('import asyncio\n', 'import asyncio\nfrom capture.hook import emit as _mlat_capture_emit\n'),
        ('        self.message_counter += 1\n',
         "        _mlat_capture_emit(self, 'input', self.receiver, msg)\n        self.message_counter += 1\n"),
        ('        return True\n',
         "        _mlat_capture_emit(self, 'connect', self.receiver, self, clock_type)\n        return True\n"),
        ('            self.coordinator.receiver_disconnect(self.receiver)\n',
         "            _mlat_capture_emit(self, 'disconnect', self.receiver)\n            self.coordinator.receiver_disconnect(self.receiver)\n"),
        ('    def write_raw(self, **kwargs):\n',
         "    def write_raw(self, **kwargs):\n        _mlat_capture_emit(self, 'outbound', self, kwargs)\n"),
        ('    def write_zlib(self, **kwargs):\n',
         "    def write_zlib(self, **kwargs):\n        _mlat_capture_emit(self, 'outbound', self, kwargs)\n"),
    )
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError('instrumentation anchor mismatch')
        text = text.replace(old, new, 1)
    ast.parse(text)  # Syntax validation before exposing a generated source copy.
    return text
