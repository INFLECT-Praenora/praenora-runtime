"""The hooks between praenora.sdk and the Python core of the runtime.

The core calls install() once with the native module it created (log, log_enabled and
cancel_requested, which act for the call that runs on the calling thread), then capture_output()
and configure_children(), and brackets each call with enter_call() and exit_call(). It reads and
writes documents with load_document() and dump_document(). Outside the core nothing is installed
and every function here still works, so modules and tests run without the runtime.
"""

import codecs
import io
import json
import logging
import sys
import threading

from . import LogLevel

_native = None
_local = threading.local()
# The streams that capture_output() installed: exit_call() ends their unfinished lines.
_streams = ()


def install(native):
    """Records the native module of the core. Called by the core, once."""
    global _native
    _native = native


def native():
    """The native module, or None outside the core."""
    return _native


def enter_call(host):
    """Marks the start of a call on this thread, made for host (a Host)."""
    stack = getattr(_local, "stack", None)
    if stack is None:
        stack = _local.stack = []
    stack.append(host)


def exit_call():
    """Marks the end of the innermost call on this thread. What the call printed without a final
    line feed, and bytes it left in the middle of a UTF-8 sequence, become a record of the call."""
    for stream in _streams:
        stream.end_call()
    _local.stack.pop()


def current_host():
    """The host of the innermost call on this thread, or None when no call runs."""
    stack = getattr(_local, "stack", None)
    return stack[-1] if stack else None


class _NullBackend:
    """The backend outside the runtime: records are dropped and nothing cancels."""

    def log(self, level, message):
        pass

    def log_enabled(self, level):
        return False

    def cancel_requested(self):
        return False


class _NativeBackend:
    """The backend inside the core: the native functions act for the current call."""

    def __init__(self, native_module):
        self._native = native_module

    def log(self, level, message):
        self._native.log(level, message)

    def log_enabled(self, level):
        return self._native.log_enabled(level)

    def cancel_requested(self):
        return self._native.cancel_requested()


def backend():
    """The backend of the Host a module gets: native inside the core, otherwise a null one."""
    if _native is None:
        return _NullBackend()
    return _NativeBackend(_native)


# Documents ----------------------------------------------------------------------------------


def _refuse_constant(name):
    raise ValueError("%s is not a JSON value" % name)


def load_document(text):
    """A JSON document (a str) as Python values; NaN and the infinities are refused, as JSON
    has neither."""
    return json.loads(text, parse_constant=_refuse_constant)


def dump_document(value):
    """A JSON-able value as a compact JSON document, non-ASCII text kept as it is. NaN and the
    infinities raise ValueError, a value JSON cannot hold TypeError."""
    return json.dumps(value, allow_nan=False, ensure_ascii=False, separators=(",", ":"))


# Output and logging -------------------------------------------------------------------------


def _log(level, message):
    native_module = _native
    if native_module is not None:
        native_module.log(int(level), message)


class _RecordBuffer(io.RawIOBase):
    """The binary side of a _RecordStream (sys.stdout.buffer): bytes are decoded as UTF-8, a
    sequence that is not UTF-8 becoming U+FFFD, each thread with a decoder of its own. A sequence
    that the bytes written so far leave unfinished waits for the next write, past a flush(), until
    the end of the call (end_call())."""

    def __init__(self, stream):
        super().__init__()
        self._stream = stream
        self._local = threading.local()

    def writable(self):
        return True

    def _decoder(self):
        decoder = getattr(self._local, "decoder", None)
        if decoder is None:
            decoder = self._local.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        return decoder

    def write(self, data):
        data = bytes(data)
        self._stream.write(self._decoder().decode(data))
        return len(data)

    def flush(self):
        self._stream.flush()

    def end_call(self):
        """Ends this thread's unfinished sequence: U+FFFD for what is left of it."""
        decoder = getattr(self._local, "decoder", None)
        if decoder is not None:
            rest = decoder.decode(b"", final=True)
            if rest:
                self._stream.write(rest)


class _RecordStream(io.TextIOBase):
    """A text stream whose lines become log records at one level: sys.stdout (INFO) and
    sys.stderr (WARN) inside the core. A record goes to the call that runs on the writing thread,
    or to the provider outside calls. Each thread has its own unfinished line, which flush() and
    the end of a call turn into a record."""

    def __init__(self, level, name):
        super().__init__()
        self._level = int(level)
        self._name = name
        self._local = threading.local()
        self.buffer = _RecordBuffer(self)

    @property
    def encoding(self):
        return "utf-8"

    @property
    def errors(self):
        return "replace"

    @property
    def name(self):
        return self._name

    def writable(self):
        return True

    def isatty(self):
        return False

    def write(self, text):
        if not isinstance(text, str):
            raise TypeError("write() argument must be str, not %s" % type(text).__name__)
        lines = (getattr(self._local, "pending", "") + text).split("\n")
        self._local.pending = lines.pop()
        for line in lines:
            _log(self._level, line)
        return len(text)

    def flush(self):
        pending = getattr(self._local, "pending", "")
        if pending:
            self._local.pending = ""
            _log(self._level, pending)

    def end_call(self):
        """Ends what this thread wrote in the call that ends: its bytes, then its line."""
        self.buffer.end_call()
        self.flush()


def _level_of(levelno):
    """The LogLevel of a logging level: each level of logging and the levels between them."""
    if levelno >= logging.CRITICAL:
        return LogLevel.CRITICAL
    if levelno >= logging.ERROR:
        return LogLevel.ERROR
    if levelno >= logging.WARNING:
        return LogLevel.WARN
    if levelno >= logging.INFO:
        return LogLevel.INFO
    if levelno >= logging.DEBUG:
        return LogLevel.DEBUG
    return LogLevel.TRACE


class _RecordHandler(logging.Handler):
    """Turns logging records into log records, "<logger name>: <message>", with the traceback of
    an exception that a record carries."""

    def __init__(self):
        super().__init__(logging.NOTSET)
        self.setFormatter(logging.Formatter("%(name)s: %(message)s"))

    def emit(self, record):
        try:
            level = _level_of(record.levelno)
            native_module = _native
            if native_module is None or not native_module.log_enabled(int(level)):
                return
            native_module.log(int(level), self.format(record))
        except Exception:
            self.handleError(record)


def capture_output():
    """Called by the core, once, after install(): sys.stdout and sys.stderr become streams whose
    lines are log records (INFO and WARN), the root logger hands every record to a handler that
    makes log records of them, and warnings go through logging (one record each)."""
    global _streams
    if _native is None:
        raise RuntimeError("capture_output() needs the native module: call install() first")
    sys.stdout = _RecordStream(LogLevel.INFO, "<stdout>")
    sys.stderr = _RecordStream(LogLevel.WARN, "<stderr>")
    _streams = (sys.stdout, sys.stderr)
    root = logging.getLogger()
    root.addHandler(_RecordHandler())
    root.setLevel(logging.NOTSET)
    logging.captureWarnings(True)


def configure_children():
    """Called by the core, once: multiprocessing starts its children with spawn (never by forking
    the runtime's process), and runs praenora.sdk._child_main as their main module, which points
    their standard output at their standard error. Without that module nothing is changed and
    RuntimeError is raised: children would print to the runtime's standard output."""
    import importlib.util
    import multiprocessing

    spec = importlib.util.find_spec("praenora.sdk._child_main")
    if spec is None:
        raise RuntimeError("praenora.sdk._child_main is not found, so the children of "
                           "multiprocessing would print to the standard output")
    multiprocessing.set_start_method("spawn", force=True)
    sys.modules["__main__"].__spec__ = spec
