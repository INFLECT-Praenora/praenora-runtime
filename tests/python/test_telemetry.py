"""Tests of the OTLP export from Python: the runtime reads the OTEL_* variables of the process
environment, and a run's metrics and logs reach a fake collector of the test's own on 127.0.0.1
(the rules of docs/testing.md#listeners: never a wildcard address, never a host name, port 0 and
the chosen port read back). Where the listener is refused (a sandbox's EPERM) the test skips,
saying "local listener refused:", or fails with PRAENORA_TESTS_REQUIRE_LISTENERS=1.

CTest sets OTEL_SDK_DISABLED for every test; this one removes it, and every other OTEL_* variable,
for its own run, and restores them afterwards.
"""

import gc
import http.server
import json
import os
import socketserver
import tempfile
import threading
import unittest

import praenora.runtime as pr
from support import FIXTURE_PATHS, listener_refused, with_captured_stderr
from test_workflow import step, write_config


class _Collector(socketserver.ThreadingMixIn, socketserver.TCPServer):
    """A fake OTLP collector: records every POST and answers 200. socketserver's own server binds
    without resolving a name (http.server.HTTPServer looks its host name up)."""

    daemon_threads = True
    allow_reuse_address = False

    def __init__(self):
        self.received = []
        self.lock = threading.Lock()
        super().__init__(("127.0.0.1", 0), _CollectorHandler)


class _CollectorHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with self.server.lock:
            self.server.received.append(
                {"path": self.path, "content_type": self.headers.get("Content-Type"),
                 "body": body})
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, format, *args):  # noqa: A002 (the base class's name)
        pass


def _attribute(attributes, key):
    for entry in attributes:
        if entry["key"] == key:
            return entry["value"]
    return None


class TelemetryTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = directory.name
        saved = {name: value for name, value in os.environ.items() if name.startswith("OTEL_")}
        for name in saved:
            del os.environ[name]

        def restore():
            for name in [name for name in os.environ if name.startswith("OTEL_")]:
                del os.environ[name]
            os.environ.update(saved)

        self.addCleanup(restore)

    @with_captured_stderr
    def test_export_to_fake_collector(self):
        try:
            collector = _Collector()
        except OSError as error:
            listener_refused(self, error)
        self.addCleanup(collector.server_close)
        thread = threading.Thread(target=collector.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(collector.shutdown)
        port = collector.server_address[1]
        os.environ["OTEL_EXPORTER_OTLP_ENDPOINT"] = f"http://127.0.0.1:{port}"
        os.environ["OTEL_EXPORTER_OTLP_TIMEOUT"] = "5000"
        os.environ["OTEL_SERVICE_NAME"] = "python-test"

        config = write_config(os.path.join(self.directory, "config"), "test.single",
                              "run: { ticks: 2 }\n", step("a", "fx-echo"))
        workflow = pr.Workflow(config, log_level="debug", **FIXTURE_PATHS)
        with workflow:
            self.assertEqual(workflow.run()["state"], "completed")
            fingerprint = workflow.fingerprint
        # The workflow, and with it its runtime, is destroyed: the exporter has flushed.
        del workflow
        gc.collect()

        with collector.lock:
            received = list(collector.received)
        paths = {request["path"] for request in received}
        self.assertIn("/v1/metrics", paths)
        self.assertIn("/v1/logs", paths)
        events = []
        for request in received:
            self.assertEqual(request["content_type"], "application/json")
            document = json.loads(request["body"].decode("utf-8"))
            signal = "resourceMetrics" if request["path"] == "/v1/metrics" else "resourceLogs"
            resource = document[signal][0]["resource"]["attributes"]
            self.assertEqual(_attribute(resource, "service.name"), {"stringValue": "python-test"})
            self.assertEqual(_attribute(resource, "process.pid"), {"intValue": str(os.getpid())})
            if signal == "resourceLogs":
                for scope in document[signal][0]["scopeLogs"]:
                    for record in scope["logRecords"]:
                        for entry in record["attributes"]:
                            self.assertTrue(entry["key"].startswith("praenora."), entry)
                        event = _attribute(record["attributes"], "praenora.event")
                        if event is not None:
                            events.append(event["stringValue"])
                        if scope["scope"]["name"] == "praenora.engine":
                            self.assertEqual(
                                _attribute(record["attributes"], "praenora.run.number"),
                                {"intValue": "1"})
                            self.assertEqual(
                                _attribute(record["attributes"], "praenora.run.fingerprint"),
                                {"stringValue": fingerprint})
            else:
                for metric in document[signal][0]["scopeMetrics"][0]["metrics"]:
                    for kind in ("sum", "gauge", "summary"):
                        for point in metric.get(kind, {}).get("dataPoints", []):
                            for entry in point["attributes"]:
                                self.assertTrue(entry["key"].startswith("praenora."), entry)
        self.assertIn("run.started", events)
        self.assertIn("run.completed", events)


if __name__ == "__main__":
    unittest.main()
