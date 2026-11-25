"""Unit tests of example-predictor-python's class with praenora.sdk's test kit, no runtime involved
(the test python.example_predictor runs them with the environment's interpreter).

example-predictor integrates each estimate before it reads the next, so an invocation that fails on
an estimate has taken the integration steps of the estimates before it, which diagnostics() counts,
and emitted no prediction; the Python module counts the same.

The interpreter may run isolated (-I, which ignores PYTHONPATH): the directories of praenora.sdk and
of this module's package come from PRAENORA_SDK_PYTHON_DIR and PRAENORA_EXAMPLE_PYTHON_DIR, which
default to the source tree's python/ and this file's directory, so that the file also runs by hand
from a checkout (python3 -I -B test_example_predictor_python.py, with numpy).
"""

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get("PRAENORA_SDK_PYTHON_DIR",
                                  os.path.join(_HERE, "..", "..", "..", "python")))
sys.path.insert(0, os.environ.get("PRAENORA_EXAMPLE_PYTHON_DIR", _HERE))

from praenora import sdk  # noqa: E402
from praenora.sdk import testing  # noqa: E402

from example_predictor_python import Predictor  # noqa: E402

PARAMETERS = {"growth_rate": 0.35, "capacity": 100.0, "horizon": 4.0, "step_size": 0.05,
              "output_interval": 0.5}
ESTIMATE = {"basis": "observation", "series_id": "S01", "variable_id": "population", "time": 0.0,
            "value": 10.0}


def state(*estimates):
    return {"time_unit": "d", "estimates": list(estimates)}


class PredictorTest(unittest.TestCase):
    def driver(self):
        driver = testing.ModuleDriver(Predictor, PARAMETERS, step="predict",
                                      capability={"id": "example.predict", "version": "0.1.0"},
                                      time={"unit": "d", "start": 0.0, "period": 1.0})
        driver.create()
        driver.initialize()
        return driver

    def steps_of(self, *estimates):
        """The integration steps that a successful invocation with `estimates` takes."""
        driver = self.driver()
        driver.invoke({"state": state(*estimates)})
        return driver.module.diagnostics()["steps"]

    def test_predicts_every_output_interval(self):
        driver = self.driver()
        result = driver.invoke({"state": state(ESTIMATE)})
        data = result["outputs"]["predictions"]["data"]
        self.assertEqual(data["issued_at"], 1.0)
        self.assertEqual([row["time"] for row in data["predictions"]],
                         [1.0 + 0.5 * j for j in range(9)])
        self.assertEqual(data["predictions"][0]["value"], 13.61990928455869)
        self.assertEqual(driver.module.diagnostics(), {"predictions": 9, "steps": 100})

    def test_a_failing_estimate_counts_the_steps_of_those_before_it(self):
        later = dict(ESTIMATE, series_id="S02", time=0.54, value=28.191)
        expected = self.steps_of(ESTIMATE, later)
        for bad in (dict(ESTIMATE, series_id="S03", time=5.0),
                    {"series_id": "S03", "variable_id": "population", "time": 0.0}):
            with self.subTest(bad=bad):
                driver = self.driver()
                with self.assertRaises(sdk.Error) as raised:
                    driver.invoke({"state": state(ESTIMATE, later, bad)})
                self.assertEqual(raised.exception.code, sdk.Status.INVALID_ARGUMENT)
                self.assertTrue(raised.exception.message.startswith("example-predictor-python: "))
                self.assertEqual(driver.module.diagnostics(),
                                 {"predictions": 0, "steps": expected})

    def test_a_failing_first_estimate_counts_no_step(self):
        driver = self.driver()
        with self.assertRaises(sdk.Error):
            driver.invoke({"state": state(dict(ESTIMATE, time=5.0), ESTIMATE)})
        self.assertEqual(driver.module.diagnostics(), {"predictions": 0, "steps": 0})

    def test_a_missing_time_unit_fails_after_the_predictions(self):
        # example-predictor reads the state's time_unit last, when it writes the result.
        driver = self.driver()
        with self.assertRaises(sdk.Error) as raised:
            driver.invoke({"state": {"estimates": [ESTIMATE]}})
        self.assertIn("the state has no string time_unit", raised.exception.message)
        self.assertEqual(driver.module.diagnostics(),
                         {"predictions": 9, "steps": self.steps_of(ESTIMATE)})


if __name__ == "__main__":
    unittest.main()
