"""Tests of the functions of praenora.runtime: the package itself, its errors, versions,
validation, modules and their contract checks, graphs, the catalog and the schemas.

CTest runs them (python.unittest) against the package in the build tree, with the environment
that support.py describes. The tests write files only to temporary directories.
"""

import json
import os
import pathlib
import pickle
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import praenora
import praenora.runtime as pr
from support import (BAD_PARAMETERS_CONFIG, EXAMPLE_CONFIG, EXAMPLE_MODULES, FIXTURE_PATHS,
                     FIXTURES, SOURCE_DIR, TEST_CATALOG, TEST_MODULES, environment,
                     needs_examples)

EXAMPLE_WORKFLOW = "example.estimate-predict@0.1.0"
# A valid configuration of the test workflow test.single (one fx-slow step).
VALID_CONFIG = FIXTURES / "configs" / "cli" / "interrupted" / "workflow.yaml"
SHA256 = re.compile(r"^[0-9a-f]{64}$")


class PackageTest(unittest.TestCase):
    def test_namespace_package(self):
        # praenora has no __init__.py: an implicit namespace package.
        self.assertTrue(not hasattr(praenora, "__file__") or praenora.__file__ is None)
        # praenora.runtime is the build tree's package.
        package_dir = pathlib.Path(environment("PYTHONPATH"), "praenora", "runtime")
        self.assertEqual(pathlib.Path(pr.__file__).parent.resolve(), package_dir.resolve())

    def test_public_names(self):
        self.assertEqual(
            sorted(pr.__all__),
            sorted([
                "PraenoraError", "Workflow", "catalog_entry", "check_module", "graph",
                "inspect_module", "list_catalog", "list_modules", "list_schemas", "read_status",
                "schema", "validate", "validate_document", "version",
            ]))
        for name in pr.__all__:
            self.assertTrue(hasattr(pr, name), name)

    def test_version_keys(self):
        info = pr.version()
        self.assertEqual(
            set(info),
            {"runtime", "sdk", "pmp", "abi", "formats", "platform", "compiler", "build_type"})
        self.assertRegex(info["runtime"], r"^[0-9]+\.[0-9]+\.[0-9]+$")
        self.assertEqual(set(info["abi"]), {"major", "minor"})
        self.assertTrue(all(isinstance(value, int) for value in info["abi"].values()))
        self.assertEqual(info["formats"]["workflow-config"], "0.1.0")


class BindingsTest(unittest.TestCase):
    def test_string_vector_holds_the_gil(self):
        # Only the facade's calls release the GIL: SWIG's own helpers of the exported
        # StringVector (its iterator, its slices) call the Python C API. A fresh interpreter in
        # development mode aborts when memory is allocated without the GIL, and setting an error
        # without it crashes any interpreter.
        code = ("from praenora.runtime import _bindings\n"
                "vector = _bindings.StringVector(['x' * 72, 'y' * 72])\n"
                "assert vector.iterator().value() == 'x' * 72\n"
                "assert list(vector[0:1]) == ['x' * 72]\n"
                "try:\n"
                "    vector[0:2**70]  # an index too large for C: reported, not a crash\n"
                "except Exception:\n"
                "    pass\n"
                "assert list(vector) == ['x' * 72, 'y' * 72]\n")
        result = subprocess.run([sys.executable, "-X", "dev", "-c", code], capture_output=True,
                                text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)


class ErrorTest(unittest.TestCase):
    def test_error_attributes(self):
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.catalog_entry("no.such-entry", "0.1.0")
        error = caught.exception
        self.assertIsInstance(error, Exception)
        self.assertEqual((error.code, error.name, error.category),
                         (405, "CATALOG_ENTRY_NOT_FOUND", "validation"))
        self.assertIn("no.such-entry", error.message)
        self.assertEqual(str(error), "CATALOG_ENTRY_NOT_FOUND: " + error.message)
        self.assertIsInstance(error.details, dict)
        self.assertIsInstance(error.location, dict)
        # Errors survive pickling, as for multiprocessing.
        copy = pickle.loads(pickle.dumps(error))
        self.assertEqual(
            (copy.code, copy.name, copy.category, copy.message, copy.details, copy.location),
            (error.code, error.name, error.category, error.message, error.details,
             error.location))
        self.assertEqual(str(copy), str(error))

    def test_missing_files_raise_io_error(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = pathlib.Path(directory, "missing")
            calls = {
                "validate": lambda: pr.validate(missing / "workflow.yaml"),
                "validate_document": lambda: pr.validate_document(missing / "document.json"),
                "check_module": lambda: pr.check_module(missing / "module.pmm.json"),
                "graph": lambda: pr.graph(config=missing / "workflow.yaml"),
                "read_status": lambda: pr.read_status(missing / "status.json"),
                "Workflow": lambda: pr.Workflow(missing / "workflow.yaml"),
            }
            for name, call in calls.items():
                with self.subTest(name):
                    with self.assertRaises(pr.PraenoraError) as caught:
                        call()
                    self.assertEqual((caught.exception.name, caught.exception.category),
                                     ("IO_ERROR", "environment"))

    def test_paths_are_str_or_path_like(self):
        self.assertTrue(pr.validate(str(VALID_CONFIG), **FIXTURE_PATHS)["ok"])
        self.assertTrue(pr.validate(VALID_CONFIG, [pathlib.Path(TEST_MODULES)],
                                    (pathlib.Path(TEST_CATALOG),))["ok"])
        # A list of paths is expected, not one path (whose characters would be paths).
        with self.assertRaises(TypeError):
            pr.list_modules(TEST_MODULES)
        with self.assertRaises(TypeError):
            pr.list_modules(pathlib.Path(TEST_MODULES))
        with self.assertRaises(TypeError):
            pr.validate_document(42)
        with self.assertRaises(TypeError):
            pr.validate_document(os.fsencode(str(VALID_CONFIG)))


class ValidateTest(unittest.TestCase):
    def test_validate_ok_and_error(self):
        report = pr.validate(VALID_CONFIG, **FIXTURE_PATHS)
        self.assertEqual(report, {"ok": True, "diagnostics": []})
        self.assertTrue(pr.validate(VALID_CONFIG, load=True, **FIXTURE_PATHS)["ok"])

        with self.assertRaises(pr.PraenoraError) as caught:
            pr.validate(BAD_PARAMETERS_CONFIG, **FIXTURE_PATHS)
        error = caught.exception
        self.assertEqual(error.name, "PARAMETERS_INVALID")
        self.assertEqual(error.category, "validation")
        self.assertEqual(error.code, 203)
        self.assertEqual(error.message, f"{BAD_PARAMETERS_CONFIG}: validation failed: 1 error(s)")
        # The first error's location, and the whole report in the details.
        self.assertEqual(pathlib.Path(error.location["file"]).name, "workflow.yaml")
        self.assertEqual(error.location["line"], 12)
        self.assertEqual(error.location["pointer"], "/steps/a/parameters/sleep_ms")
        self.assertFalse(error.details["ok"])
        diagnostics = error.details["diagnostics"]
        self.assertEqual([diagnostic["name"] for diagnostic in diagnostics], ["PARAMETERS_INVALID"])
        self.assertEqual(diagnostics[0]["location"], error.location)

    def test_validate_returns_warnings(self):
        # A manifest that cannot be read is skipped with a warning: the report is returned.
        with tempfile.TemporaryDirectory() as directory:
            pathlib.Path(directory, "broken.pmm.json").write_text("{", encoding="utf-8")
            report = pr.validate(VALID_CONFIG, [directory, TEST_MODULES], [TEST_CATALOG])
        self.assertTrue(report["ok"])
        self.assertEqual([(diagnostic["severity"], diagnostic["name"])
                          for diagnostic in report["diagnostics"]],
                         [("warning", "PARSE_ERROR")])

    def test_validate_document(self):
        valid = FIXTURES / "documents" / "valid" / "module-manifest" / "full.pmm.json"
        self.assertEqual(pr.validate_document(valid), {"ok": True, "diagnostics": []})
        invalid = FIXTURES / "documents" / "invalid" / "module-manifest" / "unknown-field.pmm.json"
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.validate_document(invalid)
        error = caught.exception
        self.assertEqual((error.name, error.category), ("SCHEMA_VIOLATION", "validation"))
        self.assertEqual(error.location["pointer"], "/entry_point")
        self.assertEqual(error.details["diagnostics"][0]["name"], "SCHEMA_VIOLATION")


class ModulesTest(unittest.TestCase):
    KEYS = {"name", "version", "manifest_path", "library_path", "os", "arch", "capabilities"}

    def test_list_and_inspect_modules(self):
        modules = pr.list_modules([TEST_MODULES])
        self.assertIn("fx-echo", [module["name"] for module in modules])
        for module in modules:
            self.assertEqual(set(module), self.KEYS)

        info = pr.inspect_module("fx-echo", module_paths=[TEST_MODULES])
        self.assertEqual(set(info), self.KEYS)
        self.assertEqual((info["name"], info["version"]), ("fx-echo", "0.1.0"))
        self.assertEqual(info["capabilities"], ["test.echo@0.1.0"])
        self.assertTrue(pathlib.Path(info["manifest_path"]).is_file())

        loaded = pr.inspect_module("fx-echo", "0.1.0", load=True, module_paths=[TEST_MODULES])
        self.assertEqual(set(loaded), self.KEYS | {"describe", "library_sha256"})
        self.assertIsInstance(loaded["describe"], dict)
        self.assertRegex(loaded["library_sha256"], SHA256)

        with self.assertRaises(pr.PraenoraError) as caught:
            pr.inspect_module("fx-nowhere", module_paths=[TEST_MODULES])
        self.assertEqual(caught.exception.name, "MODULE_NOT_FOUND")
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.inspect_module("fx-echo", "not-a-version", module_paths=[TEST_MODULES])
        self.assertEqual(caught.exception.name, "INVALID_ARGUMENT")

    def test_search_paths_default_to_the_environment(self):
        # Without module_paths the runtime searches PRAENORA_MODULE_PATH (which CTest unsets).
        self.assertNotIn("fx-echo", [module["name"] for module in pr.list_modules()])
        with mock.patch.dict(os.environ, {"PRAENORA_MODULE_PATH": TEST_MODULES}):
            self.assertIn("fx-echo", [module["name"] for module in pr.list_modules()])
            self.assertEqual(pr.inspect_module("fx-echo")["name"], "fx-echo")


class CheckModuleTest(unittest.TestCase):
    @needs_examples
    def test_check_module_example_passes(self):
        manifest = pathlib.Path(EXAMPLE_MODULES, "example-estimator", "example-estimator.pmm.json")
        report = pr.check_module(manifest)
        self.assertEqual(report["kind"], "module-check-report")
        self.assertTrue(report["passed"])
        statuses = {check["id"]: check["status"] for check in report["checks"]}
        self.assertEqual(len(statuses), 17)
        self.assertEqual(statuses["describe.identity"], "pass")
        self.assertEqual(statuses["invoke.expected_outputs"], "pass")
        self.assertNotIn("fail", statuses.values())

    def test_check_module_failure_raises_with_report(self):
        manifest = pathlib.Path(TEST_MODULES, "fx-identity-mismatch",
                                "fx-identity-mismatch.pmm.json")
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.check_module(manifest, catalog_paths=[TEST_CATALOG])
        error = caught.exception
        self.assertEqual((error.name, error.category), ("FAILED", "execution"))
        self.assertEqual(error.message, "1 of 17 contract checks failed: describe.identity")
        self.assertFalse(error.details["passed"])
        self.assertEqual([check["id"] for check in error.details["checks"]
                          if check["status"] == "fail"], ["describe.identity"])

    def test_check_module_with_vectors(self):
        manifest = pathlib.Path(TEST_MODULES, "fx-echo", "fx-echo.pmm.json")
        vectors = FIXTURES / "configs" / "cli" / "echo-vectors.json"
        report = pr.check_module(manifest, vectors, catalog_paths=[TEST_CATALOG])
        statuses = {check["id"]: check["status"] for check in report["checks"]}
        self.assertEqual(statuses["invoke.expected_outputs"], "pass")

    def test_check_module_catalog_paths(self):
        # The capability test.echo of fx-echo is in the test catalog only: without it, the
        # contract checks cannot find it. The catalog paths are keyword-only, and a lone path is
        # refused like everywhere else.
        manifest = pathlib.Path(TEST_MODULES, "fx-echo", "fx-echo.pmm.json")
        self.assertTrue(pr.check_module(manifest, catalog_paths=[TEST_CATALOG])["passed"])
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.check_module(manifest)
        self.assertEqual(caught.exception.name, "FAILED")
        self.assertIn("manifest.semantics", caught.exception.message)
        with self.assertRaises(TypeError):
            pr.check_module(manifest, None, [TEST_CATALOG])
        with self.assertRaises(TypeError):
            pr.check_module(manifest, catalog_paths=TEST_CATALOG)


class GraphTest(unittest.TestCase):
    def test_graph_formats(self):
        graph = pr.graph(workflow=EXAMPLE_WORKFLOW)
        self.assertIsInstance(graph, dict)
        self.assertEqual(graph["workflow"],
                         {"name": "example.estimate-predict", "version": "0.1.0"})
        self.assertEqual(graph["schedule"], ["estimate", "predict"])
        self.assertEqual(pr.graph(config=EXAMPLE_CONFIG), graph)
        self.assertEqual(pr.graph(EXAMPLE_CONFIG, format="json"), graph)

        text = pr.graph(config=EXAMPLE_CONFIG, format="text")
        self.assertIsInstance(text, str)
        self.assertIn("schedule:", text)
        dot = pr.graph(workflow=EXAMPLE_WORKFLOW, format="dot")
        self.assertIsInstance(dot, str)
        self.assertTrue(dot.startswith("digraph"))
        # AsyncAPI is returned as text: the committed document, byte for byte.
        asyncapi = pr.graph(workflow=EXAMPLE_WORKFLOW, format="asyncapi")
        self.assertIsInstance(asyncapi, str)
        self.assertEqual(json.loads(asyncapi)["asyncapi"], "3.0.0")
        committed = (SOURCE_DIR / "spec" / "asyncapi" /
                     "example.estimate-predict-0.1.0.asyncapi.json")
        self.assertEqual(asyncapi.encode("utf-8"), committed.read_bytes())

        chain = pr.graph(workflow="test.chain@0.1.0", catalog_paths=[TEST_CATALOG])
        self.assertEqual(chain["schedule"], ["a", "b"])

        for arguments in ({"workflow": EXAMPLE_WORKFLOW, "format": "svg"}, {},
                          {"config": EXAMPLE_CONFIG, "workflow": EXAMPLE_WORKFLOW},
                          {"workflow": "example.estimate-predict"}):
            with self.subTest(arguments=arguments):
                with self.assertRaises(pr.PraenoraError) as caught:
                    pr.graph(**arguments)
                self.assertEqual(caught.exception.name, "INVALID_ARGUMENT")
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.graph(workflow="no.such-workflow@0.1.0")
        self.assertEqual(caught.exception.name, "WORKFLOW_NOT_FOUND")


class CatalogTest(unittest.TestCase):
    def test_catalog_and_schemas(self):
        entries = pr.list_catalog()
        for entry in entries:
            self.assertEqual(set(entry), {"kind", "id", "version", "status", "summary"})
        self.assertIn(("workflow-definition", "example.estimate-predict", "0.1.0"),
                      [(entry["kind"], entry["id"], entry["version"]) for entry in entries])
        kinds = {"workflows": "workflow-definition", "capabilities": "capability-definition",
                 "messages": "message-type"}
        for kind, entry_kind in kinds.items():
            with self.subTest(kind=kind):
                selected = pr.list_catalog(kind)
                self.assertTrue(selected)
                self.assertEqual({entry["kind"] for entry in selected}, {entry_kind})
        self.assertNotIn("test.chain", [entry["id"] for entry in pr.list_catalog("workflows")])
        self.assertIn("test.chain", [entry["id"] for entry in
                                     pr.list_catalog("workflows", catalog_paths=[TEST_CATALOG])])
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.list_catalog("widgets")
        self.assertEqual(caught.exception.name, "INVALID_ARGUMENT")

        entry = pr.catalog_entry("example.estimate-predict", "0.1.0")
        self.assertEqual((entry["kind"], entry["name"]),
                         ("workflow-definition", "example.estimate-predict"))
        self.assertEqual(pr.catalog_entry("test.chain", "0.1.0",
                                          catalog_paths=[TEST_CATALOG])["name"], "test.chain")

        schemas = pr.list_schemas()
        self.assertEqual(len(schemas), 16)
        self.assertEqual(schemas, sorted(schemas))
        self.assertIn("module-manifest@0.1", schemas)
        schema = pr.schema("module-manifest")
        self.assertEqual(schema["$id"], "urn:praenora:schema:module-manifest:0.1")
        self.assertEqual(pr.schema("module-manifest", "0.1"), schema)
        self.assertEqual(pr.schema("module-manifest", format_version="0.1.0"), schema)
        with self.assertRaises(pr.PraenoraError) as caught:
            pr.schema("no-such-kind")
        self.assertEqual(caught.exception.name, "FORMAT_VERSION_UNSUPPORTED")


if __name__ == "__main__":
    unittest.main()
