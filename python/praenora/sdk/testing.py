"""A test kit for Python modules: drive a module class as the runtime does, without the runtime.

    from praenora.sdk import testing

    driver = testing.ModuleDriver(Estimator, {"gain": 2.0}, step="estimate", seed=7)
    driver.create()
    driver.initialize()
    result = driver.invoke({"x": 3.0}, tick=0)   # port -> payload; the module reads ["x"]["data"]
    driver.finalize()

The kit needs only the standard library. It takes the runtime's path through the class: it
creates the module with the class method Module._praenora_adapter, as the runtime does, so a class
that does not derive from Module fails here as there, and calls the methods of the object that
returns. Every document it sends is written as JSON and read back, as the runtime sends it (a tuple
arrives as a list), and so is every result it gets: a result the runtime cannot write (a numpy
integer, float32 or bool_, or an array, NaN or an infinity, a set) fails here as it fails there,
with Error(Status.INVALID_ARGUMENT) and the runtime's message, and one above 16 MiB with
Error(Status.RESOURCE_EXHAUSTED). The documents have the fields the runtime sends (protocol.md),
with the inputs wrapped in envelopes.

The kit makes every call on the calling thread. The runtime makes every call of an instance on one
thread too, the instance's own, so what Python keeps per thread (context variables such as numpy's
error state, threading.local values) lasts from one call to the next in both.
"""

from . import Error, Host, Status, _runtime

# The largest document, in bytes of UTF-8, that the runtime takes from a module
# (PRAENORA_PYTHON_MAX_DOCUMENT_SIZE of praenora/sdk/python.h).
MAX_DOCUMENT_SIZE = 16 * 1024 * 1024


class FakeHost:
    """A host that records the log records in logs, a list of (level, message), and reports
    cancellation after cancel_after polls of cancel_requested (never, when None).

    In the runtime the host acts for the call that runs on the calling thread: on a thread that
    module code starts (threading, concurrent.futures), log() records go to the provider's log and
    cancel_requested() is always false. FakeHost answers alike on every thread, so a test cannot
    tell: log and poll on the thread of the call."""

    def __init__(self, cancel_after=None):
        self.logs = []
        self.cancel_after = cancel_after
        self.polls = 0

    def log(self, level, message):
        self.logs.append((level, message))

    def log_enabled(self, level):
        return True

    def cancel_requested(self):
        self.polls += 1
        return self.cancel_after is not None and self.polls > self.cancel_after


def _exception_text(error):
    """The exception as the runtime names it: "<Type>: <message>", the type's module before it
    but for builtins."""
    cls = type(error)
    name = cls.__qualname__
    if cls.__module__ not in ("builtins", "__main__"):
        name = cls.__module__ + "." + name
    message = str(error)
    return "%s: %s" % (name, message) if message else name


def _sent(value, what):
    """value as the module gets it from the runtime: written as JSON and read back. A value that
    JSON cannot hold raises the TypeError or ValueError of the JSON writer, naming what."""
    try:
        return _runtime.load_document(_runtime.dump_document(value))
    except (TypeError, ValueError) as error:
        raise type(error)("%s is not a JSON document: %s" % (what, error)) from error


def _received(value, what):
    """value as the runtime takes it from the module: written as JSON, as the runtime writes it
    (Error INVALID_ARGUMENT when it cannot be, RESOURCE_EXHAUSTED above MAX_DOCUMENT_SIZE), and
    read back."""
    try:
        text = _runtime.dump_document(value)
    except (TypeError, ValueError, RecursionError) as error:
        raise Error(Status.INVALID_ARGUMENT, "%s is not a JSON document: %s"
                    % (what, _exception_text(error))) from error
    size = len(text.encode("utf-8", "surrogatepass"))
    if size > MAX_DOCUMENT_SIZE:
        raise Error(Status.RESOURCE_EXHAUSTED,
                    "%s is %d bytes, more than the 16 MiB (%d bytes) that a document may take"
                    % (what, size, MAX_DOCUMENT_SIZE),
                    {"size": size, "limit": MAX_DOCUMENT_SIZE})
    return _runtime.load_document(text)


class ModuleDriver:
    """Runs a module class through create, initialize, invoke and finalize, and diagnostics and
    the self test, as the runtime does.

    parameters is the step's parameters, step its name and seed the run's seed. host is a FakeHost
    (one is made when omitted); its records are in driver.logs. Once created, the module (the
    instance of cls) is driver.module, and the object the runtime calls, which holds it, is
    driver.adapter.

    The documents have the shape the runtime sends (the PMP create, initialize and invocation
    documents). capability, module and time can be given; the defaults are a capability
    "test.capability" 0.1.0, a module "test-module" 0.1.0 and a time base of unit "1", start 0.0
    and period 1.0.
    """

    PMP_VERSION = "0.1.0"

    def __init__(self, cls, parameters=None, step="s", seed=1, host=None, capability=None,
                 module=None, time=None):
        self.cls = cls
        self.parameters = {} if parameters is None else parameters
        self.step = step
        self.seed = seed
        self.host = FakeHost() if host is None else host
        self.capability = capability or {"id": "test.capability", "version": "0.1.0"}
        self.module_identity = module or {"name": "test-module", "version": "0.1.0"}
        self.time = time or {"unit": "1", "start": 0.0, "period": 1.0}
        self.module = None
        self.adapter = None
        self.initialize_document = None

    @property
    def logs(self):
        return self.host.logs

    def create_document(self):
        return {
            "pmp_version": self.PMP_VERSION,
            "step": self.step,
            "capability": self.capability,
            "module": self.module_identity,
            "parameters": self.parameters,
        }

    def _adapter(self, args):
        # A class that does not derive from Module has no _praenora_adapter: AttributeError, as
        # the runtime's create reports.
        entry = self.cls._praenora_adapter
        return entry(_sent(args, "the arguments of create"), host=Host(self.host))

    def _call(self, method, document):
        if self.adapter is None:
            raise RuntimeError("ModuleDriver.%s(): call create() first" % method)
        sent = None if document is None else _sent(document, "the document of " + method)
        return _received(getattr(self.adapter, method)(sent), "the result of " + method)

    def create(self):
        """Creates the module as the runtime does: Module._praenora_adapter of the class with
        {"create": <create document>}, which constructs the class with a CreateContext (the host
        and the create document). Returns the module."""
        self.adapter = self._adapter({"create": self.create_document()})
        self.module = self.adapter.module
        return self.module

    def initialize(self):
        """Calls initialize with {pmp_version, run: {fingerprint, seed}, time}."""
        self.initialize_document = {
            "pmp_version": self.PMP_VERSION,
            "run": {"fingerprint": "0" * 64, "seed": self.seed},
            "time": self.time,
        }
        self._call("initialize", self.initialize_document)

    def logical_time(self, tick):
        """The end of the tick's window: start + (tick + 1) * period."""
        return self.time["start"] + (tick + 1) * self.time["period"]

    def invoke(self, inputs, tick=0):
        """Calls invoke for a tick and returns the result document, as the runtime reads it.

        inputs maps each input port to its payload; the driver wraps each payload in an envelope
        as the runtime does ({pmp_version, id, type, type_version, source, tick, logical_time,
        data}), so the module reads document["inputs"][port]["data"].
        """
        logical_time = self.logical_time(tick)
        envelopes = {}
        for port, data in inputs.items():
            source = "inputs." + port
            envelopes[port] = {
                "pmp_version": self.PMP_VERSION,
                "id": "%s:%d:%s" % ("0" * 16, tick, source),
                "type": "test.message",
                "type_version": "0.1.0",
                "source": source,
                "tick": tick,
                "logical_time": logical_time,
                "data": data,
            }
        return self._call("invoke", {
            "pmp_version": self.PMP_VERSION,
            "step": self.step,
            "tick": tick,
            "logical_time": logical_time,
            "inputs": envelopes,
        })

    def finalize(self):
        """Calls finalize."""
        self._call("finalize", None)

    def diagnostics(self):
        """Calls diagnostics and returns its document ({} for a module that returns None)."""
        return self._call("diagnostics", None)

    def self_test(self):
        """Runs the class's self test as the runtime does, with no instance: the class method
        self_test(host) of an object made with {"create": null}. Returns its report document;
        Error(Status.UNSUPPORTED) for a class without self_test. Needs no create()."""
        adapter = self._adapter({"create": None})
        return _received(adapter.self_test(None), "the result of self_test")
