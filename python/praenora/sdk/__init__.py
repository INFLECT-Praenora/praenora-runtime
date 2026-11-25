"""praenora.sdk: the adapter of Python modules of the Praenora runtime.

A module is a class derived from Module. The runtime creates it with a CreateContext (the host and
the create document) and calls initialize, invoke and finalize with documents, which are dicts of
JSON values. This package needs only the standard library, so it imports in any interpreter, and
praenora.sdk.testing drives a module class without the runtime.

    from praenora import sdk

    class Estimator(sdk.Module):
        def __init__(self, context):
            super().__init__(context)
            self.gain = context.document["parameters"]["gain"]

        def invoke(self, document):
            return {"outputs": {"estimate": {"data": self.gain}}}
"""

import enum

__all__ = [
    "Cancelled",
    "CreateContext",
    "Error",
    "Host",
    "LogLevel",
    "Module",
    "Status",
]


class LogLevel(enum.IntEnum):
    """The levels of a log record (PRAENORA_LOG_* of abi.h)."""

    TRACE = 0
    DEBUG = 1
    INFO = 2
    WARN = 3
    ERROR = 4
    CRITICAL = 5


class Status(enum.IntEnum):
    """The status codes of the module ABI (PRAENORA_STATUS_* of status.h)."""

    OK = 0
    UNKNOWN = 1
    INVALID_ARGUMENT = 2
    INVALID_STATE = 3
    NOT_FOUND = 4
    UNSUPPORTED = 5
    RESOURCE_EXHAUSTED = 6
    CANCELLED = 7
    TIMEOUT = 8
    IO_ERROR = 9
    INTERNAL = 10
    FAILED = 11
    DATA_INVALID = 12


def _rebuild_error(cls, state):
    """Unpickles an Error or a subclass: no constructor call, the state restored as it was."""
    error = cls.__new__(cls)
    Exception.__init__(error, state["message"])
    error.__dict__.update(state)
    return error


class Error(Exception):
    """A failure with a status code, a message and optional details (a JSON-able dict).

    code is a Status or an integer: module-defined codes start at 10000. The runtime turns the
    exception into the failed call's error document.
    """

    def __init__(self, code, message, details=None):
        super().__init__(message)
        try:
            self.code = Status(code)
        except ValueError:
            self.code = int(code)
        self.message = message
        self.details = details

    def __str__(self):
        return str(self.message)

    def __reduce__(self):
        # Errors cross process boundaries (multiprocessing pools). The default reduction calls
        # the class with args, which holds only the message and which a constructor of another
        # signature (a subclass's, or this one's) refuses; rebuild without calling the
        # constructor and restore every attribute, a subclass's extra ones included.
        return (_rebuild_error, (type(self), dict(self.__dict__)))


class Cancelled(Error):
    """Raised by module code that saw a cancel request: the call ends with CANCELLED."""

    def __init__(self, message="cancelled", details=None):
        super().__init__(Status.CANCELLED, message, details)


class Host:
    """What a module may ask of the runtime. backend does the work: log(level, message),
    log_enabled(level) and cancel_requested()."""

    def __init__(self, backend):
        self._backend = backend

    def log(self, level, message):
        """Writes a log record of the module's step at a LogLevel."""
        self._backend.log(int(level), str(message))

    def log_enabled(self, level):
        """True when a record at that level would be kept: skip building costly messages."""
        return bool(self._backend.log_enabled(int(level)))

    def cancel_requested(self):
        """True once the run asked the call to end: poll it in long loops and raise Cancelled."""
        return bool(self._backend.cancel_requested())


class CreateContext:
    """What a module's constructor gets: host (a Host) and document (the create document: the
    step's name, its parameters and the run's identity, as a dict)."""

    def __init__(self, host, document):
        self.host = host
        self.document = document


class Module:
    """The base class of Python modules. Override invoke; the other methods are optional.

    A class method self_test(host) -> {"passed": bool, "checks": [...]} may be defined when the
    module is built with a self test.
    """

    def __init__(self, context):
        self.context = context

    def initialize(self, document):
        """Starts a run: document holds the run's fingerprint and seed, and the time settings."""

    def invoke(self, document):
        """One call of the step: the invocation document in, the result document out."""
        raise Error(Status.UNSUPPORTED, "the module does not define invoke")

    def finalize(self):
        """Ends the run; release what initialize or invoke acquired."""

    def diagnostics(self):
        """A dict of facts about the module's state, for status reports."""
        return {}

    @classmethod
    def _praenora_adapter(cls, args, host=None):
        """The runtime's entry point for the class; module code never calls or overrides it.

        Returns the adapter object of praenora.sdk._adapter for cls. args is {"create": <create
        document>}; with "create" absent or null (or args null) the adapter is class-only and
        serves self_test() alone. The runtime calls it by the name
        "<package.module:Class>._praenora_adapter", so the class's own top-level package is the
        one the runtime imports, and the one whose location it checks. host is not given by the
        runtime; praenora.sdk.testing gives the Host of its FakeHost.
        """
        from . import _adapter  # _adapter imports this module

        return _adapter.adapter_of(cls, args, host)
