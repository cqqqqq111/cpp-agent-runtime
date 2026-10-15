"""Exclusive, bounded leases for reusable HTTP clients and their event loops.

One lease runs synchronously on the calling thread: streaming callbacks retain
their original thread affinity. An idle session can be leased by another thread,
but its event loop/client are never run by two callers at once. No worker thread
or detached request survives a timeout.
"""
import asyncio
import threading
import time
import weakref

_pools = weakref.WeakSet()


def _require_sync_context():
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return
    raise RuntimeError('Synchronous LLM transport cannot be called/closed from a running event loop')


class _Session:
    def __init__(self, factory):
        self.loop = asyncio.new_event_loop()
        self.factory = factory
        self.client = None

    def run(self, operation, deadline):
        async def invoke():
            if self.client is None:
                self.client = self.factory()
            return await operation(self.client)

        async def bounded():
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                raise TimeoutError('LLM request deadline exceeded before dispatch')
            return await asyncio.wait_for(invoke(), remaining)

        # create_task inside run_until_complete copies the current caller's
        # contextvars, rather than keeping the context of the first requester.
        return self.loop.run_until_complete(bounded())

    def close(self):
        try:
            if self.client is not None:
                self.loop.run_until_complete(self.client.close())
        finally:
            self.client = None
            try:
                pending = asyncio.all_tasks(self.loop)
                for task in pending:
                    task.cancel()
                if pending:
                    self.loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
                self.loop.run_until_complete(self.loop.shutdown_asyncgens())
                self.loop.run_until_complete(self.loop.shutdown_default_executor())
            finally:
                self.loop.close()


class TransportPool:
    def __init__(self, factory, max_sessions=4):
        if not 1 <= max_sessions <= 32:
            raise ValueError('LLM_MAX_SESSIONS must be between 1 and 32')
        self.factory = factory
        self.max_sessions = max_sessions
        self._condition = threading.Condition()
        self._idle = []
        self._active = 0
        self._closed = False
        self._closing = False
        self._close_done = False
        _pools.add(self)

    def _acquire(self, deadline):
        with self._condition:
            while True:
                if self._closed:
                    raise RuntimeError('LLM transport is closed')
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    raise TimeoutError('LLM deadline exceeded waiting for a transport session')
                if self._idle:
                    session = self._idle.pop()
                elif self._active < self.max_sessions:
                    session = _Session(self.factory)
                else:
                    self._condition.wait(remaining)
                    continue
                self._active += 1
                return session

    def run(self, operation, timeout_ms=0):
        _require_sync_context()
        if timeout_ms < 0:
            raise ValueError('Negative request timeout')
        deadline = time.monotonic() + timeout_ms / 1000 if timeout_ms else None
        session = self._acquire(deadline)
        try:
            return session.run(operation, deadline)
        finally:
            # wait_for has fully awaited cancellation, and streaming's finally
            # has closed its response, before this session becomes available.
            with self._condition:
                self._idle.append(session)
                self._active -= 1
                self._condition.notify_all()

    def close(self):
        _require_sync_context()
        with self._condition:
            if self._closing:
                self._condition.wait_for(lambda: self._close_done)
                return
            self._closing = True
            self._closed = True
            self._condition.notify_all()
            self._condition.wait_for(lambda: self._active == 0)
            sessions, self._idle = self._idle, []
        error = None
        try:
            for session in sessions:
                try:
                    session.close()
                except BaseException as exc:
                    if error is None:
                        error = exc
        finally:
            with self._condition:
                self._close_done = True
                self._condition.notify_all()
        if error is not None:
            raise error


def close_all_transports():
    error = None
    for pool in list(_pools):
        try:
            pool.close()
        except BaseException as exc:
            if error is None:
                error = exc
    if error is not None:
        raise error
