# SPDX-License-Identifier: AGPL-3.0-or-later
"""Opt-in adapter for the pinned service startup; no installation or injection.

The deployment caller must first verify/stage the complete source/native bundle
and satisfy the public source offer. This adapter only owns observer lifecycle.
It uses the existing utility-subtask extension point, before listeners start.
"""
import hashlib
import logging
from pathlib import Path

from .session import CaptureSession

MAIN_SHA256 = '5176a8411dda442126b625bc1d3b1e58e1fd9fb5897ef8a3412fa8b363897fbd'


class ObserverSubtask:
    def __init__(self, session):
        self.session = session

    async def start(self):
        pass

    def close(self):
        self.session.stop()

    async def wait_closed(self):
        if not await self.session.wait_closed():
            # Capture must not make ordinary service shutdown fail.
            logging.getLogger(__name__).warning('Capture writer shutdown timed out')


def server_class(main, coordinator, tracker, directory, *, enabled=False, duration=120):
    """Return an isolated subclass; never mutate an imported class/module.

    Drift disables capture, not the original service. Disabled mode performs no
    source/directory IO. Original parsing, startup, signals and shutdown run
    unchanged; a finally boundary also stops capture after partial startup.
    """
    base = main.MlatServer
    if not enabled:
        return base
    try:
        if type(enabled) is not bool:
            raise ValueError('explicit capture enable required')
        path = Path(main.__file__)
        if path.stat().st_size > 1024*1024:
            raise ValueError('main source size')
        if hashlib.sha256(path.read_bytes()).hexdigest() != MAIN_SHA256:
            raise ValueError('main source drift')
    except Exception:
        logging.getLogger(__name__).warning('Capture startup disabled (source gate)')
        return base

    class ObservedServer(base):
        def run(self):
            self._capture_session = None
            try:
                return super().run()
            finally:
                session = self._capture_session
                if session is not None:
                    try:
                        session.stop()
                        # run() owns the event loop synchronously. On normal
                        # shutdown the observer was already awaited. If startup
                        # failed, still drain the bounded writer before exit.
                        if not session.recorder._done.is_set():
                            if self.loop.is_closed():
                                if not session.recorder.close(timeout=10):
                                    logging.getLogger(__name__).warning('Capture writer shutdown timed out')
                            else:
                                self.loop.run_until_complete(ObserverSubtask(session).wait_closed())
                    except Exception:
                        # Do not mask the original service startup exception or
                        # print its potentially sensitive arguments.
                        logging.getLogger(__name__).warning('Capture final cleanup failed')

        def make_util_subtasks(self, args):
            tasks = super().make_util_subtasks(args)
            # This is called by the hash-pinned run() after Coordinator is
            # constructed, before output/client subtasks or start() execute.
            session = CaptureSession.start(self.coordinator, self.loop,
                ((coordinator, 'coordinator'), (tracker, 'tracker')),
                directory, enabled=True, duration=duration)
            if session is not None:
                self._capture_session = session
                tasks.append(ObserverSubtask(session))
            return tasks

    return ObservedServer
