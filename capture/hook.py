# SPDX-License-Identifier: AGPL-3.0-or-later
"""Offline source-hook boundary. Installation/production startup not provided."""
from .lifecycle import Lifecycle
from .reference import output, OUTPUTS, server_number
from . import status as status_schema
from .sbs import line as sbs_line
from .solution import make as make_solution


class Hook(Lifecycle):
    def __init__(self, recorder, **kwargs):
        super().__init__(recorder, **kwargs)
        self._native_record = getattr(recorder, '_native_record', None)
        # Only concrete built-in implementation objects: subclasses/custom
        # attribute semantics retain the Python method below. These dictionaries
        # are mutated in place by lifecycle/stop, never replaced internally.
        from .recorder import Recorder
        if self._native_record is not None and type(self) is Hook and type(recorder) is Recorder:
            from functools import partial
            from _mlat_capture_native import input_context
            self.input = partial(input_context, recorder.__dict__, self.active, self._input_status)

    def _input_status(self, status, receiver, message):
        """Cold path for the native input context; same policy as input()."""
        rec = self.recorder
        try:
            if status == -1: raise ValueError('input schema')
            if status == 6:
                self._fail(); return False
            if status == 5:
                return rec.record_input(self.identity(receiver), message)
            if status == 4: rec.stop('duration')
            elif status in (2, 3):
                rec.dropped += 1
                rec.invalidate('record_limit' if status == 3 else 'queue_limit')
            self.clear()
            return False
        except Exception:
            rec.dropped += 1
            rec.invalidate('input_schema')
            self.clear()
            return False

    def input(self, receiver, message):
        if self._native_record is None:
            return super().input(receiver, message)
        rec = self.recorder
        if not rec.active:
            self.clear(); return False
        try:
            status = self._native_record(rec._native_queue, self.active, rec.started_mono,
                                         rec.deadline, receiver, message)
            if status == 1: return True
            if status == 6:
                self._fail(); return False
            if status == 5:
                return rec.record_input(self.identity(receiver), message)
            if status == 4: rec.stop('duration')
            elif status in (2, 3):
                rec.dropped += 1
                rec.invalidate('record_limit' if status == 3 else 'queue_limit')
            self.clear()
            return False
        except Exception:
            rec.dropped += 1
            rec.invalidate('input_schema')
            self.clear()
            return False

    def location(self, receiver):
        if not self.recorder.active or id(receiver) not in self.active:
            return False  # initial constructor location is included by connect
        try:
            from .schema import number
            llh = tuple(number(x) for x in receiver.position_llh)
            if len(llh) != 3 or not -90 <= llh[0] <= 90 or not -180 <= llh[1] <= 360 or not -1000 <= llh[2] <= 10000:
                raise ValueError("location bounds")
            return self.recorder.submit("location", self.identity(receiver), llh)
        except Exception:
            self._fail()
            return False

    def solution(self, *args):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            payload = make_solution(*args, identity=self.known_identity)
            return self.recorder.submit('solution', 0, payload)
        except Exception:
            self._fail()
            return False

    def clear(self):
        super().clear()
        if hasattr(self, '_sbs_sinks'):
            self._sbs_sinks.clear()

    def sbs_connect(self, sink, filtered):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            if not hasattr(self, '_sbs_sinks'):
                self._sbs_sinks = {}
                self._sbs_sequence = 0
            if type(filtered) is not bool or id(sink) in self._sbs_sinks or len(self._sbs_sinks) >= 1024 or self._sbs_sequence >= 65536:
                raise ValueError('SBS sink bound')
            identity = self._sbs_sequence + 1
            if not self.recorder.submit('sbs_connect', 0, (identity, filtered)):
                return False
            self._sbs_sequence = identity
            self._sbs_sinks[id(sink)] = (sink, identity)
            return True
        except Exception:
            self._fail()
            return False

    def sbs_output(self, sink, text):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            identity = self._sbs_sinks[id(sink)][1]
            return self.recorder.submit('sbs_output', 0, (identity, sbs_line(text)))
        except Exception:
            self._fail()
            return False

    def sbs_disconnect(self, sink):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            identity = self._sbs_sinks.pop(id(sink))[1]
            return self.recorder.submit('sbs_disconnect', 0, identity)
        except Exception:
            self._fail()
            return False

    def status(self, coordinator, wall, aircraft, clients, sync):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            tables = {'aircraft': aircraft, 'clients': clients, 'sync': sync}
            if any(type(v) is not dict or len(v) > status_schema.TABLE_LIMITS[k] for k, v in tables.items()):
                raise ValueError('status snapshot bounds')
            snapshot = getattr(self, '_snapshot', 0) + 1
            self._snapshot = snapshot
            def user_id(user): return self.identity(coordinator.usernames[user])
            def uid_id(uid): return self.identity(coordinator.receivers[uid])
            if not self.recorder.submit('status_begin', 0, dict(id=snapshot, wall=wall,
                    counts={k: len(v) for k, v in tables.items()})):
                return False
            # One bounded row at a time; never duplicate whole status tables.
            for table, rows in tables.items():
                if table == 'aircraft' and self.recorder._native_aircraft_batch is not None:
                    if type(coordinator.receivers) is not dict or len(coordinator.receivers) > 4096:
                        raise ValueError('status identity map bound')
                    uid_map = {uid: self.identity(r) for uid, r in coordinator.receivers.items()}
                    batch = []
                    for pair in rows.items():
                        batch.append(pair)
                        if len(batch) == 64:
                            if not self.recorder.record_aircraft_batch(snapshot, batch, uid_map): return False
                            batch.clear()
                    if batch and not self.recorder.record_aircraft_batch(snapshot, batch, uid_map): return False
                    continue
                for key, value in rows.items():
                    if not self.recorder.active:
                        return False
                    if table == 'aircraft':
                        if not self.recorder.record_aircraft(snapshot, key, value, uid_id):
                            return False
                        continue
                    elif table == 'clients':
                        key = user_id(key)
                        sanitized = status_schema.client(value)
                    else:
                        key = user_id(key)
                        sanitized = status_schema.sync(value, user_id)
                    if not self.recorder.submit('status_row', 0, (snapshot, table, key, sanitized)):
                        return False
            return self.recorder.submit('status_end', 0, snapshot)
        except Exception:
            self._fail()
            return False

    def connect(self, receiver, client, clock_type):
        # TCP-only pilot qualification cannot silently omit UDP datagrams.
        # This invalidates capture, not UDP service or its accepted handshake.
        if self.recorder.active and client.use_udp:
            self.recorder.invalidate('udp_hook_unimplemented')
            self.clear()
            return None
        return super().connect(receiver, client, clock_type)

    def outbound(self, client, message):
        if not self.recorder.active:
            self.clear()
            return False
        try:
            # Handshake/denial may precede connect. Never inspect their values,
            # raw source offers, MOTD, auth, or free-form denial strings.
            if ('compress' in message or 'deny' in message) and id(client.receiver) not in self.active:
                return False
            if not OUTPUTS.intersection(message):
                raise ValueError('unsupported outbound message')
            payload = output(message, scalar=server_number)
            return self.recorder.submit('client_output', self.identity(client.receiver), payload)
        except Exception:
            self._fail()
            return False


def emit(client, action, *args):
    """Fault-isolated outer guard, including broken/unexpected hook callbacks.

    Source code calls this alongside its original operations, never instead of
    them. Normal server exceptions retain their original propagation behavior.
    """
    hook = None
    try:
        hook = getattr(client.coordinator, '_mlat_capture', None)
        if hook is not None:
            getattr(hook, action)(*args)
    except Exception:
        try:
            if hook is not None:
                hook.recorder.invalidate('hook_error')
                hook.clear()
        except Exception:
            pass


def emit_coordinator(coordinator, action, *args):
    hook = None
    try:
        hook = getattr(coordinator, '_mlat_capture', None)
        if hook is not None:
            getattr(hook, action)(*args)
    except Exception:
        try:
            if hook is not None:
                hook.recorder.invalidate('hook_error')
                hook.clear()
        except Exception:
            pass


# Preserve independent Python boundaries for differential tests and platforms
# without the extension. Source instrumentation imports the selected functions.
_python_emit, _python_emit_coordinator = emit, emit_coordinator
try:
    from _mlat_capture_native import emit, emit_coordinator
except ImportError:
    pass
