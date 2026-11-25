"""Tests of a Python step run from the Python front end: the front end runs in an interpreter of
its own (the one praenora.runtime is built for), and a process holds one interpreter, so the
provider python cannot start in it. A Python step in the front end's process fails, saying to run
it in a module host; the same step in a module host (isolation mode process) runs.

They need the provider python in the build tree (PRAENORA_WITH_PYTHON_ENVIRONMENT), and skip
without it, and the Python fixture module PYTHON_FIXTURE_MODULE, which serves the capability of
fx-echo.
"""

import os
import pathlib
import tempfile
import unittest

import praenora.runtime as pr
from support import BUILD_PROVIDERS, TEST_CATALOG, TEST_PYTHON_MODULES, needs_python_environment

# The Python fixture module the tests bind their step to, and where it is.
PYTHON_FIXTURE_MODULE = "fx-py-echo"
PYTHON_FIXTURE_PATHS = {"module_paths": [TEST_PYTHON_MODULES], "catalog_paths": [TEST_CATALOG],
                        "provider_paths": [BUILD_PROVIDERS]}


def write_config(directory, isolation):
    """A configuration of the test workflow test.single whose one step, a, is bound to
    PYTHON_FIXTURE_MODULE and runs in `isolation` ("in-process" or "process")."""
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "source.json").write_text('{"value": 42}\n', encoding="utf-8", newline="\n")
    mode = "process" if isolation == "process" else "in-process"
    config = directory / "workflow.yaml"
    config.write_text("kind: workflow-config\n"
                      'format_version: "0.1.0"\n'
                      'workflow: { name: test.single, version: "0.1.0" }\n'
                      f"run: {{ ticks: 1, isolation: {{ mode: {mode} }} }}\n"
                      "inputs:\n"
                      "  source: { source: json-document, path: source.json }\n"
                      "steps:\n"
                      "  a:\n"
                      f'    module: {{ name: {PYTHON_FIXTURE_MODULE}, version: "0.1.0" }}\n'
                      "    parameters: {}\n",
                      encoding="utf-8", newline="\n")
    return config


@needs_python_environment
class PythonEnvironmentTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = pathlib.Path(directory.name)

    def test_in_process_python_step_refused(self):
        # This process runs the interpreter that imported praenora.runtime: the provider python
        # refuses to start a second one in it.
        config = write_config(self.directory / "in-process", "in-process")
        with pr.Workflow(config, **PYTHON_FIXTURE_PATHS,
                         output_dir=self.directory / "out") as workflow:
            with self.assertRaises(pr.PraenoraError) as caught:
                workflow.run()
        error = caught.exception
        self.assertEqual(error.name, "LIBRARY_LOAD_FAILED")
        self.assertEqual(error.details["reason"], "start")
        self.assertEqual(error.details["cause"]["code"], 5)  # PRAENORA_STATUS_UNSUPPORTED
        self.assertIn("module host", str(error))
        self.assertIn("isolation mode process", str(error))

    @unittest.skipIf(os.name == "nt", "steps cannot run in module hosts on Windows")
    def test_module_host_python_step_runs(self):
        # The module host is a process of its own, without another interpreter.
        config = write_config(self.directory / "process", "process")
        with pr.Workflow(config, **PYTHON_FIXTURE_PATHS,
                         output_dir=self.directory / "out") as workflow:
            self.assertEqual(workflow.run()["state"], "completed")


if __name__ == "__main__":
    unittest.main()
