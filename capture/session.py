# SPDX-License-Identifier: AGPL-3.0-or-later
"""Startup-only finite observer ownership; no installer or running-process injection."""
import asyncio
import logging
import math
import threading
from .hook import Hook
from .randomness import RecordingRandom
from .recorder import Recorder

_MISSING=object()


class CaptureSession:
    @classmethod
    def start(cls, coordinator, loop, modules, directory, *, enabled=False,
              duration=120, native=True, min_free_bytes=1024*1024*1024):
        """Explicit opt-in; fail-open setup. Call before any receiver connects.

        modules is ((coordinator_module,'coordinator'),(tracker_module,'tracker')).
        No credentials/free-form configuration is accepted or logged here.
        """
        if not enabled:return None
        self=cls();self.coordinator=coordinator;self.loop=loop
        self.owner=threading.get_ident();self.recorder=None;self.hook=None
        self.bindings=[];self.timer=None;self.detached=False;self.original_hook=_MISSING
        try:
            if type(enabled) is not bool or type(duration) not in (int,float) or not math.isfinite(duration) or not 0<duration<=120:
                raise ValueError('capture duration/enable bound')
            if type(native) is not bool or type(min_free_bytes) is not int or not 0<=min_free_bytes<2**63:
                raise ValueError('capture runtime/disk bound')
            if loop.is_running() and asyncio.get_running_loop() is not loop:
                raise ValueError('capture startup loop ownership')
            if type(coordinator.receivers) is not dict or coordinator.receivers:
                raise ValueError('capture requires clean startup')
            self.original_hook=getattr(coordinator,'_mlat_capture',_MISSING)
            if self.original_hook is not _MISSING and self.original_hook is not None:raise ValueError('existing capture owner')
            if type(modules) not in (tuple,list) or len(modules)!=2:raise ValueError('RNG module scope')
            if {stream for module,stream in modules}!={'coordinator','tracker'} or modules[0][0] is modules[1][0]:
                raise ValueError('RNG stream scope')
            originals=[(module,stream,module.random) for module,stream in modules]
            if any(not all(callable(getattr(delegate,key,None)) for key in ('random','sample','shuffle')) for module,stream,delegate in originals):
                raise ValueError('RNG delegate interface')
            self.recorder=Recorder(directory,duration=duration,native=native,compress=False,min_free_bytes=min_free_bytes)
            self.hook=Hook(self.recorder)
            for module,stream,delegate in originals:
                wrapper=RecordingRandom(delegate,self.recorder,self.hook.known_identity,stream)
                self.bindings.append((module,delegate,wrapper))
                module.random=wrapper
            coordinator._mlat_capture=self.hook
            self.timer=loop.call_later(.25,self._poll)
            return self
        except BaseException as error:
            if self.recorder is not None:self.recorder.invalidate('startup_error')
            self._detach()
            if not isinstance(error,Exception):raise
            logging.getLogger(__name__).warning('Capture startup disabled (fixed safety diagnostic)')
            return None

    def _check_owner(self):
        if threading.get_ident()!=self.owner:raise RuntimeError('capture session requires owner thread')

    def _detach(self):
        self._check_owner()
        if self.detached:return
        self.detached=True
        if self.timer is not None:self.timer.cancel();self.timer=None
        # Never overwrite observers/delegates installed by a newer owner.
        if self.hook is not None and getattr(self.coordinator,'_mlat_capture',_MISSING) is self.hook:
            try:
                if self.original_hook is _MISSING:del self.coordinator._mlat_capture
                else:self.coordinator._mlat_capture=self.original_hook
            except Exception:logging.getLogger(__name__).warning('Capture observer detach failed')
        for module,delegate,wrapper in reversed(self.bindings):
            try:
                if getattr(module,'random',_MISSING) is wrapper:module.random=delegate
            except Exception:logging.getLogger(__name__).warning('Capture RNG detach failed')
        self.bindings.clear()
        if self.hook is not None:self.hook.clear()

    def _poll(self):
        self._check_owner();self.timer=None
        if self.detached:return
        if getattr(self.coordinator,'_mlat_capture',_MISSING) is not self.hook:
            self.recorder.invalidate('session_replaced')
        if not self.recorder.active:self._detach();return
        try:self.timer=self.loop.call_later(.25,self._poll)
        except Exception:
            self.recorder.invalidate('session_timer_error');self._detach()

    def stop(self):
        self._check_owner()
        if self.recorder is not None:self.recorder.stop('requested')
        self._detach()

    async def wait_closed(self,timeout=10):
        """Poll writer completion without a blocking wait or extra worker thread."""
        self._check_owner()
        if asyncio.get_running_loop() is not self.loop:raise RuntimeError('capture session loop mismatch')
        if not 0<=timeout<=30:raise ValueError('shutdown timeout bound')
        deadline=self.loop.time()+timeout
        while self.recorder is not None and not self.recorder._done.is_set():
            remaining=deadline-self.loop.time()
            if remaining<=0:return False
            await asyncio.sleep(min(.02,remaining))
        return True
