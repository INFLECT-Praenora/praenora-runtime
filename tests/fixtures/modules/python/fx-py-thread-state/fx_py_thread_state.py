"""fx-py-thread-state: what Python keeps per thread, set in initialize and read by each invoke: a
context variable, numpy's error state (a context variable too) and a threading.local value, with a
count of the invocations that each invoke raises; and whether the call runs on the thread that
created the instance."""

import contextvars
import threading

import numpy

from praenora import sdk

_VALUE = contextvars.ContextVar("fx_py_thread_state_value")
_LOCAL = threading.local()


class ThreadState(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.thread = threading.get_ident()

    def initialize(self, document):
        _VALUE.set("initialized")
        numpy.seterr(divide="raise")
        _LOCAL.value = "initialized"
        _LOCAL.invocations = 0

    def invoke(self, document):
        _LOCAL.invocations = getattr(_LOCAL, "invocations", 0) + 1
        return {"outputs": {"out": {"data": {
            "context": _VALUE.get(None),
            "numpy_divide": numpy.geterr()["divide"],
            "local": getattr(_LOCAL, "value", None),
            "invocations": _LOCAL.invocations,
            "same_thread": threading.get_ident() == self.thread,
        }}}}
