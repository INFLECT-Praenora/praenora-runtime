"""Threads: what Python keeps per thread (a context variable, decimal's context and a
threading.local value), set by one call and read by the next, and a value whose release when the
thread state is cleared is recorded."""

import contextvars
import decimal
import threading

_VALUE = contextvars.ContextVar("core_fixtures_threads_value")
_LOCAL = threading.local()
_RELEASED = []


class _Marker:
    """Records its release in _RELEASED."""

    def __init__(self, name):
        self.name = name

    def __del__(self):
        _RELEASED.append(self.name)


class Threads:
    def __init__(self, document):
        pass

    def set(self, document):
        value = document["value"]
        _VALUE.set(value)
        _LOCAL.value = value
        decimal.getcontext().prec = value
        return None

    def get(self, document):
        return {
            "context": _VALUE.get(None),
            "local": getattr(_LOCAL, "value", None),
            "precision": decimal.getcontext().prec,
        }

    def keep(self, document):
        """Keeps a marker named document["name"] as this thread's threading.local value."""
        _LOCAL.marker = _Marker(document["name"])
        return None

    def released(self, document):
        return document["name"] in _RELEASED
