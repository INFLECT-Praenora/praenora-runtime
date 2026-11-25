"""Raising: each method raises an exception of another kind."""

from praenora import sdk


class Raising:
    def __init__(self, document):
        pass

    def value_error(self, document):
        raise ValueError("the value is %s" % (document,))

    def sdk_error(self, document):
        raise sdk.Error(sdk.Status.DATA_INVALID, "x", {"k": 1})

    def cancelled(self, document):
        raise sdk.Cancelled()

    def memory_error(self, document):
        raise MemoryError("no room for %s" % (document,))


class FailsToConstruct:
    def __init__(self, document):
        raise ValueError("cannot be constructed")
