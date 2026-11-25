"""Tests of praenora.sdk, the pure-Python package of Python modules.

The package is imported from the directory that PRAENORA_SDK_PYTHON_DIR names (the interpreter runs
isolated, which ignores PYTHONPATH), and status.h is read from PRAENORA_STATUS_HEADER.
"""

import multiprocessing
import os
import pickle
import re
import sys
import unittest

sys.path.insert(0, os.environ["PRAENORA_SDK_PYTHON_DIR"])

from praenora import sdk  # noqa: E402
from praenora.sdk import _adapter, _runtime, testing  # noqa: E402


class Gain(sdk.Module):
    """A module of the tests: scales the inputs by a parameter."""

    created = []

    def __init__(self, context):
        super().__init__(context)
        self.gain = context.document["parameters"]["gain"]
        self.seed = None
        self.finalized = False
        Gain.created.append(self)

    def initialize(self, document):
        self.seed = document["run"]["seed"]

    def invoke(self, document):
        host = self.context.host
        host.log(sdk.LogLevel.INFO, "invoked")
        return {"outputs": {"y": self.gain * document["inputs"]["x"]["data"]}, "seed": self.seed,
                "document": document}

    def finalize(self):
        self.finalized = True

    @classmethod
    def self_test(cls, host):
        return {"passed": True, "checks": []}


class NeedsParameters(sdk.Module):
    """A module whose constructor cannot run without a create document."""

    def __init__(self, context):
        super().__init__(context)
        self.value = context.document["parameters"]["required"]

    @classmethod
    def self_test(cls, host):
        host.log(sdk.LogLevel.INFO, "self test")
        return {"passed": True, "checks": [{"name": "host", "passed": True}]}


class Poller(sdk.Module):
    def invoke(self, document):
        polls = 0
        while not self.context.host.cancel_requested():
            polls += 1
            if polls > 100:
                raise AssertionError("cancellation never arrived")
        raise sdk.Cancelled()


class ParameterError(sdk.Error):
    """An error class of a module: another constructor signature and an extra attribute."""

    def __init__(self, name):
        super().__init__(sdk.Status.INVALID_ARGUMENT, "bad parameter " + name, {"name": name})
        self.name = name


def raise_parameter_error(name):
    raise ParameterError(name)


def raise_cancelled(_):
    raise sdk.Cancelled("stop", {"why": 1})


class NotAModule:
    def __init__(self, context):
        pass


class Opaque:
    """A value that JSON cannot hold, as numpy's integers and arrays cannot be held either."""


class Returns(sdk.Module):
    """A module of the tests whose invoke returns the value that the parameter "make" names, and
    whose diagnostics returns None."""

    VALUES = {
        "opaque": lambda: Opaque(),
        "nan": lambda: float("nan"),
        "tuple": lambda: (1, 2),
        "large": lambda: "x" * (17 << 20),
    }

    def invoke(self, document):
        make = self.context.document["parameters"]["make"]
        return {"outputs": {"y": {"data": self.VALUES[make]()}}}

    def diagnostics(self):
        return None


class AdapterTest(unittest.TestCase):
    def test_create_and_invoke(self):
        create = {"parameters": {"gain": 3}, "step": "s"}
        adapter = _adapter.create({"class": __name__ + ":Gain", "create": create})
        adapter.initialize({"run": {"seed": 7}})
        document = {"inputs": {"x": {"data": 2}}}
        result = adapter.invoke(document)
        self.assertEqual(result["outputs"], {"y": 6})
        self.assertEqual(result["seed"], 7)
        self.assertEqual(adapter.diagnostics(), {})
        self.assertEqual(adapter.self_test(None), {"passed": True, "checks": []})
        self.assertIsNone(adapter.finalize())
        self.assertTrue(Gain.created[-1].finalized)

    def test_class_not_a_module_refused(self):
        with self.assertRaises(TypeError) as caught:
            _adapter.create({"class": __name__ + ":NotAModule", "create": {}})
        self.assertIn("NotAModule", str(caught.exception))

    def test_class_missing_refused(self):
        with self.assertRaises(AttributeError):
            _adapter.create({"class": __name__ + ":Absent", "create": {}})
        with self.assertRaises(ValueError):
            _adapter.create({"class": "no-colon", "create": {}})

    def test_class_only_adapter_serves_self_test(self):
        for args in ({"class": __name__ + ":NeedsParameters"},
                     {"class": __name__ + ":NeedsParameters", "create": None}):
            adapter = _adapter.create(args)
            result = adapter.self_test(None)
            self.assertEqual(result["passed"], True)
            with self.assertRaises(sdk.Error) as caught:
                adapter.invoke({})
            self.assertEqual(caught.exception.code, sdk.Status.INVALID_STATE)
        # The same class refuses to be constructed without parameters.
        with self.assertRaises(KeyError):
            _adapter.create({"class": __name__ + ":NeedsParameters", "create": {}})

    def test_class_method_is_the_runtimes_entry(self):
        # The runtime creates the adapter through the class: Class._praenora_adapter(args).
        create = {"parameters": {"gain": 5}, "step": "s"}
        adapter = Gain._praenora_adapter({"create": create})
        self.assertIsInstance(adapter, _adapter.Adapter)
        adapter.initialize({"run": {"seed": 3}})
        self.assertEqual(adapter.invoke({"inputs": {"x": {"data": 2}}})["outputs"], {"y": 10})
        self.assertIs(Gain.created[-1].context.document, create)
        # Without a create document (null, absent, or no arguments at all) it is class-only.
        for args in ({"create": None}, {}, None):
            adapter = NeedsParameters._praenora_adapter(args)
            self.assertEqual(adapter.self_test(None)["passed"], True)
            with self.assertRaises(sdk.Error) as caught:
                adapter.invoke({})
            self.assertEqual(caught.exception.code, sdk.Status.INVALID_STATE)
        # A class that does not derive from Module has no such entry.
        self.assertFalse(hasattr(NotAModule, "_praenora_adapter"))

    def test_self_test_needs_a_class_method(self):
        adapter = _adapter.create({"class": __name__ + ":Poller", "create": {}})
        with self.assertRaises(sdk.Error) as caught:
            adapter.self_test(None)
        self.assertEqual(caught.exception.code, sdk.Status.UNSUPPORTED)


class TestingTest(unittest.TestCase):
    def test_driver_runs_module(self):
        driver = testing.ModuleDriver(Gain, {"gain": 4}, step="est", seed=11)
        driver.create()
        driver.initialize()
        result = driver.invoke({"x": 5}, tick=2)
        self.assertEqual(result["outputs"]["y"], 20)
        self.assertEqual(result["seed"], 11)
        # The documents have the shape of protocol.md.
        invocation = result["document"]
        self.assertEqual(invocation["tick"], 2)
        self.assertEqual(invocation["logical_time"], 3.0)
        self.assertEqual(invocation["step"], "est")
        envelope = invocation["inputs"]["x"]
        self.assertEqual(envelope["data"], 5)
        self.assertEqual(envelope["source"], "inputs.x")
        self.assertEqual(envelope["tick"], 2)
        for field in ("pmp_version", "id", "type", "type_version", "logical_time"):
            self.assertIn(field, envelope)
        create = driver.module.context.document
        for field in ("pmp_version", "step", "capability", "module", "parameters"):
            self.assertIn(field, create)
        self.assertEqual(driver.initialize_document["time"],
                         {"unit": "1", "start": 0.0, "period": 1.0})
        self.assertEqual(driver.initialize_document["run"]["seed"], 11)
        self.assertIn("pmp_version", driver.initialize_document)
        driver.finalize()
        self.assertTrue(driver.module.finalized)
        self.assertIn((sdk.LogLevel.INFO, "invoked"), driver.host.logs)

    def test_cancel_after(self):
        host = testing.FakeHost(cancel_after=3)
        self.assertEqual([host.cancel_requested() for _ in range(5)],
                         [False, False, False, True, True])
        driver = testing.ModuleDriver(Poller, {}, host=host)
        driver.create()
        driver.initialize()
        with self.assertRaises(sdk.Cancelled) as caught:
            driver.invoke({})
        self.assertEqual(caught.exception.code, sdk.Status.CANCELLED)

    def test_fake_host_logs_and_levels(self):
        host = testing.FakeHost()
        host.log(sdk.LogLevel.WARN, "careful")
        self.assertEqual(host.logs, [(sdk.LogLevel.WARN, "careful")])
        self.assertTrue(host.log_enabled(sdk.LogLevel.TRACE))
        self.assertFalse(host.cancel_requested())

    def test_testing_needs_no_runtime(self):
        self.assertIsNone(_runtime.native())
        self.assertEqual(type(_runtime.backend()).__name__, "_NullBackend")
        self.assertIsNone(_runtime.current_host())

    def test_driver_creates_through_the_runtimes_entry(self):
        # The runtime creates a module with the class method Module._praenora_adapter: a class
        # that does not derive from Module fails in the kit as in the runtime.
        with self.assertRaises(AttributeError) as caught:
            testing.ModuleDriver(NotAModule).create()
        self.assertIn("_praenora_adapter", str(caught.exception))
        driver = testing.ModuleDriver(Gain, {"gain": 2})
        self.assertIsNone(driver.module)
        module = driver.create()
        self.assertIsInstance(module, Gain)
        self.assertIs(driver.module, module)
        self.assertIsInstance(driver.adapter, _adapter.Adapter)

    def test_results_are_written_as_json(self):
        # What the runtime cannot write as JSON fails as it fails there: INVALID_ARGUMENT, with
        # the runtime's message; above 16 MiB, RESOURCE_EXHAUSTED.
        for make, raised in (("opaque", "TypeError: "), ("nan", "ValueError: ")):
            driver = testing.ModuleDriver(Returns, {"make": make})
            driver.create()
            driver.initialize()
            with self.assertRaises(sdk.Error) as caught:
                driver.invoke({})
            self.assertEqual(caught.exception.code, sdk.Status.INVALID_ARGUMENT, make)
            self.assertTrue(str(caught.exception).startswith(
                "the result of invoke is not a JSON document: " + raised), caught.exception)
        driver = testing.ModuleDriver(Returns, {"make": "large"})
        driver.create()
        with self.assertRaises(sdk.Error) as caught:
            driver.invoke({})
        self.assertEqual(caught.exception.code, sdk.Status.RESOURCE_EXHAUSTED)
        self.assertIn("16 MiB", str(caught.exception))
        # What JSON holds differently comes back as the runtime reads it: a tuple as a list.
        driver = testing.ModuleDriver(Returns, {"make": "tuple"})
        driver.create()
        self.assertEqual(driver.invoke({}), {"outputs": {"y": {"data": [1, 2]}}})

    def test_documents_are_written_as_json(self):
        # The module reads what the runtime would send: the documents written as JSON and read
        # back (a tuple becomes a list); a value that JSON cannot hold is refused.
        driver = testing.ModuleDriver(Gain, {"gain": 1})
        driver.create()
        driver.initialize()
        result = driver.invoke({"x": (1, 2)})
        self.assertEqual(result["document"]["inputs"]["x"]["data"], [1, 2])
        with self.assertRaises(TypeError):
            driver.invoke({"x": Opaque()})
        with self.assertRaises(TypeError):
            testing.ModuleDriver(Gain, {"gain": Opaque()}).create()

    def test_diagnostics_and_self_test(self):
        driver = testing.ModuleDriver(Returns, {"make": "tuple"})
        driver.create()
        self.assertEqual(driver.diagnostics(), {})  # None, as the runtime reports it
        # The self test runs as the runtime runs it: the class method self_test(host) of a
        # class-only adapter, which constructs no instance; its records go to the kit's host.
        driver = testing.ModuleDriver(NeedsParameters)
        report = driver.self_test()
        self.assertEqual(report, {"passed": True, "checks": [{"name": "host", "passed": True}]})
        self.assertIsNone(driver.module)
        self.assertIn((sdk.LogLevel.INFO, "self test"), driver.logs)
        with self.assertRaises(sdk.Error) as caught:
            testing.ModuleDriver(Poller).self_test()
        self.assertEqual(caught.exception.code, sdk.Status.UNSUPPORTED)


class RuntimeHooksTest(unittest.TestCase):
    def test_enter_and_exit_call_nest(self):
        first, second = testing.FakeHost(), testing.FakeHost()
        _runtime.enter_call(first)
        _runtime.enter_call(second)
        self.assertIs(_runtime.current_host(), second)
        _runtime.exit_call()
        self.assertIs(_runtime.current_host(), first)
        _runtime.exit_call()
        self.assertIsNone(_runtime.current_host())


class FakeNative:
    """Stands for the core's native module: keeps the records it is given."""

    def __init__(self):
        self.records = []

    def log(self, level, message):
        self.records.append((level, message))

    def log_enabled(self, level):
        return level >= sdk.LogLevel.DEBUG

    def cancel_requested(self):
        return False


class CoreHooksTest(unittest.TestCase):
    """The parts of praenora.sdk._runtime that the core installs, run with a fake native module
    and without replacing this process's own streams."""

    def setUp(self):
        self.native = FakeNative()
        self.saved = _runtime._native
        _runtime._native = self.native

    def tearDown(self):
        _runtime._native = self.saved

    def test_documents_are_compact_json_without_nan(self):
        self.assertEqual(_runtime.dump_document({"a": [1, 2.5], "t": "caf\u00e9"}),
                         '{"a":[1,2.5],"t":"caf\u00e9"}')
        for value in (float("nan"), float("inf"), [float("-inf")]):
            with self.assertRaises(ValueError):
                _runtime.dump_document(value)
        with self.assertRaises(TypeError):
            _runtime.dump_document({1, 2})
        self.assertEqual(_runtime.load_document('{"x":[1,null]}'), {"x": [1, None]})
        for text in ("NaN", "[Infinity]", '{"x":-Infinity}'):
            with self.assertRaises(ValueError):
                _runtime.load_document(text)

    def test_stream_writes_one_record_per_line(self):
        stream = _runtime._RecordStream(sdk.LogLevel.WARN, "<stderr>")
        self.assertEqual(stream.write("first\nsec"), 9)
        self.assertEqual(self.native.records, [(3, "first")])
        stream.write("ond\n\nthird")
        self.assertEqual(self.native.records, [(3, "first"), (3, "second"), (3, "")])
        stream.flush()
        self.assertEqual(self.native.records[-1], (3, "third"))
        stream.flush()
        self.assertEqual(len(self.native.records), 4, "nothing left to flush")
        self.assertEqual(stream.encoding, "utf-8")
        self.assertTrue(stream.writable())
        self.assertFalse(stream.isatty())
        with self.assertRaises(TypeError):
            stream.write(b"bytes")

    def test_stream_buffer_decodes_with_replacement(self):
        stream = _runtime._RecordStream(sdk.LogLevel.INFO, "<stdout>")
        stream.buffer.write(b"caf\xc3")  # the first byte of a two-byte sequence ...
        stream.buffer.write(b"\xa9 \xff\n")  # ... its second, and a byte that is never UTF-8
        self.assertEqual(self.native.records, [(2, "caf\u00e9 \ufffd")])

    def test_each_thread_has_its_own_line(self):
        import threading

        stream = _runtime._RecordStream(sdk.LogLevel.INFO, "<stdout>")
        stream.write("main ")
        thread = threading.Thread(target=lambda: (stream.write("other"), stream.flush()))
        thread.start()
        thread.join()
        stream.write("line\n")
        self.assertEqual(self.native.records, [(2, "other"), (2, "main line")])

    def test_handler_maps_levels_and_names_the_logger(self):
        import logging

        handler = _runtime._RecordHandler()
        logger = logging.getLogger("core.hooks.test")
        for levelno, level in ((5, 0), (logging.DEBUG, 1), (logging.INFO, 2),
                               (logging.WARNING, 3), (logging.ERROR, 4), (logging.CRITICAL, 5),
                               (99, 5)):
            record = logger.makeRecord(logger.name, levelno, __file__, 1, "at %d", (levelno,),
                                       None)
            handler.handle(record)
        self.assertEqual(self.native.records,
                         [(1, "core.hooks.test: at 10"), (2, "core.hooks.test: at 20"),
                          (3, "core.hooks.test: at 30"), (4, "core.hooks.test: at 40"),
                          (5, "core.hooks.test: at 50"), (5, "core.hooks.test: at 99")],
                         "level 5 maps to TRACE, which the fake native does not keep")

    def test_exit_call_ends_the_threads_unfinished_lines(self):
        stream = _runtime._RecordStream(sdk.LogLevel.INFO, "<stdout>")
        saved = _runtime._streams
        _runtime._streams = (stream,)
        try:
            _runtime.enter_call(testing.FakeHost())
            stream.write("no line feed")
            _runtime.exit_call()
        finally:
            _runtime._streams = saved
        self.assertEqual(self.native.records, [(2, "no line feed")])

    def test_a_partial_byte_sequence_ends_with_its_call(self):
        stream = _runtime._RecordStream(sdk.LogLevel.INFO, "<stdout>")
        saved = _runtime._streams
        _runtime._streams = (stream,)
        try:
            _runtime.enter_call(testing.FakeHost())
            try:
                stream.buffer.write(b"first call \xc3")  # a sequence the call never ends
                # A flush makes a record of the complete text and keeps the unfinished
                # sequence, which the next bytes may complete.
                stream.buffer.flush()
                after_flush = list(self.native.records)
            finally:
                _runtime.exit_call()
            _runtime.enter_call(testing.FakeHost())
            try:
                stream.buffer.write(b"second call\n")
            finally:
                _runtime.exit_call()
        finally:
            _runtime._streams = saved
        self.assertEqual(after_flush, [(2, "first call ")])
        # The end of the call ends the sequence; the next call's record is clean.
        self.assertEqual(self.native.records,
                         [(2, "first call "), (2, "\ufffd"), (2, "second call")])

    def test_configure_children_refuses_a_missing_child_main(self):
        import importlib.util

        main = sys.modules["__main__"]
        saved_spec = getattr(main, "__spec__", None)
        saved_find_spec = importlib.util.find_spec
        importlib.util.find_spec = lambda name, package=None: None
        try:
            with self.assertRaises(RuntimeError) as raised:
                _runtime.configure_children()
        finally:
            importlib.util.find_spec = saved_find_spec
            main.__spec__ = saved_spec
        self.assertIn("praenora.sdk._child_main", str(raised.exception))


class ErrorTest(unittest.TestCase):
    def test_error_carries_code_and_details(self):
        error = sdk.Error(sdk.Status.DATA_INVALID, "bad data", {"path": "x"})
        self.assertEqual(error.code, 12)
        self.assertEqual(error.message, "bad data")
        self.assertEqual(error.details, {"path": "x"})
        self.assertIn("bad data", str(error))
        self.assertEqual(sdk.Error(5, "m").details, None)
        self.assertEqual(sdk.Error(10001, "module code").code, 10001)

    def test_errors_survive_pickling(self):
        error = pickle.loads(pickle.dumps(sdk.Error(sdk.Status.DATA_INVALID, "bad", {"a": [1]})))
        self.assertIs(type(error), sdk.Error)
        self.assertEqual((error.code, error.message, error.details),
                         (sdk.Status.DATA_INVALID, "bad", {"a": [1]}))
        module_error = pickle.loads(pickle.dumps(sdk.Error(10002, "mine")))
        self.assertEqual(module_error.code, 10002)
        cancelled = pickle.loads(pickle.dumps(sdk.Cancelled("stop", {"why": 1})))
        self.assertIs(type(cancelled), sdk.Cancelled)
        self.assertEqual((cancelled.code, cancelled.message, cancelled.details),
                         (sdk.Status.CANCELLED, "stop", {"why": 1}))
        self.assertEqual(pickle.loads(pickle.dumps(sdk.Cancelled())).message, "cancelled")

    def test_subclass_with_its_own_signature_pickles(self):
        error = pickle.loads(pickle.dumps(ParameterError("gain")))
        self.assertIs(type(error), ParameterError)
        self.assertEqual(error.name, "gain")
        self.assertEqual((error.code, error.message, error.details),
                         (sdk.Status.INVALID_ARGUMENT, "bad parameter gain", {"name": "gain"}))
        self.assertEqual(str(error), "bad parameter gain")
        self.assertEqual(error.args, ("bad parameter gain",))

    def test_errors_cross_a_spawn_pool(self):
        context = multiprocessing.get_context("spawn")
        pool = context.Pool(1)
        try:
            with self.assertRaises(ParameterError) as caught:
                pool.apply_async(raise_parameter_error, ("gain",)).get(timeout=60)
            self.assertEqual(caught.exception.name, "gain")
            self.assertEqual(caught.exception.code, sdk.Status.INVALID_ARGUMENT)
            with self.assertRaises(sdk.Cancelled) as cancelled:
                pool.apply_async(raise_cancelled, (0,)).get(timeout=60)
            self.assertEqual(cancelled.exception.details, {"why": 1})
        finally:
            pool.terminate()
            pool.join()

    def test_cancelled_defaults(self):
        error = sdk.Cancelled()
        self.assertEqual(error.code, sdk.Status.CANCELLED)
        self.assertEqual(error.message, "cancelled")
        self.assertIsInstance(error, sdk.Error)
        self.assertEqual(sdk.Cancelled("stop").message, "stop")


class StatusTest(unittest.TestCase):
    def test_values_match_status_h(self):
        with open(os.environ["PRAENORA_STATUS_HEADER"], encoding="utf-8") as header:
            text = header.read()
        found = dict(re.findall(r"#define PRAENORA_STATUS_(\w+)\s+(\d+)\b", text))
        found.pop("MODULE_BASE")
        self.assertEqual(len(found), 13)
        self.assertEqual({status.name: int(status.value) for status in sdk.Status},
                         {name: int(value) for name, value in found.items()})

    def test_log_levels(self):
        self.assertEqual([(level.name, int(level)) for level in sdk.LogLevel],
                         [("TRACE", 0), ("DEBUG", 1), ("INFO", 2), ("WARN", 3), ("ERROR", 4),
                          ("CRITICAL", 5)])


if __name__ == "__main__":
    unittest.main()
