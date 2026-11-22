"""What the tests of tests/python share: the environment that CTest sets for python.unittest,
the paths of the fixtures, and a way to keep the runtime's log records out of the test output.

CTest's environment names the fixture modules (PRAENORA_TEST_MODULES_DIR), the fixture service
providers (PRAENORA_TEST_PROVIDERS_DIR), the test catalog (PRAENORA_TEST_CATALOG_DIR), the example
configuration (PRAENORA_EXAMPLE_CONFIG) and, when the examples are built, the example modules
(PRAENORA_EXAMPLE_MODULES_DIR). It sets PRAENORA_LOG_LEVEL to off, so the runtime logs nothing
unless a test asks for it.
"""

import contextlib
import functools
import os
import pathlib
import sys
import tempfile
import unittest

SOURCE_DIR = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = SOURCE_DIR / "tests" / "fixtures"


def environment(name):
    """The value of the environment variable `name`, which CTest sets."""
    value = os.environ.get(name, "")
    if not value:
        raise RuntimeError(f"{name} is not set; run these tests with ctest (python.unittest)")
    return value


TEST_MODULES = environment("PRAENORA_TEST_MODULES_DIR")
TEST_PROVIDERS = environment("PRAENORA_TEST_PROVIDERS_DIR")
TEST_CATALOG = environment("PRAENORA_TEST_CATALOG_DIR")
EXAMPLE_CONFIG = environment("PRAENORA_EXAMPLE_CONFIG")
EXAMPLE_MODULES = os.environ.get("PRAENORA_EXAMPLE_MODULES_DIR", "")
needs_examples = unittest.skipUnless(EXAMPLE_MODULES, "the example modules are not built")

# The fixture modules and the test catalog, as keyword arguments.
FIXTURE_PATHS = {"module_paths": [TEST_MODULES], "catalog_paths": [TEST_CATALOG]}
# A configuration of the test workflow test.single whose fx-slow step sets sleep_ms below its
# minimum: PARAMETERS_INVALID at /steps/a/parameters/sleep_ms.
BAD_PARAMETERS_CONFIG = FIXTURES / "configs" / "cli" / "bad-parameters" / "workflow.yaml"
# A configuration of test.single whose input's data file does not exist: TARGET_DATA_UNAVAILABLE
# at /inputs/source/path.
MISSING_DATA_CONFIG = FIXTURES / "configs" / "cli" / "missing-data" / "workflow.yaml"
# The fixture provider test-echo, which serves the test service test.echo@0, as a provider search
# path.
TEST_ECHO_PROVIDER = str(pathlib.Path(TEST_PROVIDERS, "test-echo"))
# A configuration of test.single whose fx-service step requires test.echo@0: SERVICE_UNAVAILABLE
# at /requires/services/0 of fx-service's manifest without a provider of it, valid with
# TEST_ECHO_PROVIDER.
SERVICE_CONFIG = FIXTURES / "configs" / "cli" / "service" / "workflow.yaml"
# A provider search path that holds the manifest of a provider built for runtime 0.0.1, which no
# runtime of this repository can use, and in its subdirectory shadowed/ another version of it, which
# the first shadows.
UNUSABLE_PROVIDERS = FIXTURES / "configs" / "cli" / "providers"


@contextlib.contextmanager
def captured_stderr():
    """Sends what the process writes to its standard error (file descriptor 2, where the runtime
    logs) to a temporary file while the block runs. Yields a function that returns what was
    written so far."""
    sys.stderr.flush()
    saved = os.dup(2)
    with tempfile.TemporaryFile() as capture:
        os.dup2(capture.fileno(), 2)

        def written():
            # The reads move the shared file offset to the end, where the next write goes.
            os.lseek(capture.fileno(), 0, os.SEEK_SET)
            data = b""
            while True:
                chunk = os.read(capture.fileno(), 1 << 16)
                if not chunk:
                    return data.decode("utf-8", errors="replace")
                data += chunk

        try:
            yield written
        finally:
            sys.stderr.flush()
            os.dup2(saved, 2)
            os.close(saved)


def with_captured_stderr(test):
    """Decorates the test method `test` to run inside captured_stderr(), with self.stderr set to
    the function that returns what was written. The capture ends when the method returns or raises,
    before unittest reports the outcome: from Python 3.11 on, unittest writes a failed test's FAIL
    or ERROR as soon as the exception leaves the method, before tearDown and the cleanups run, so a
    capture that they end would hold that word. A failed subtest's word, which unittest writes
    when the subtest ends, would still land in the capture."""

    @functools.wraps(test)
    def run_captured(self, *args, **kwargs):
        with captured_stderr() as written:
            self.stderr = written
            return test(self, *args, **kwargs)

    return run_captured
