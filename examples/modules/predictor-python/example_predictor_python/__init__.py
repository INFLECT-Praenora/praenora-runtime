"""example-predictor-python: the predict step of the example workflow (capability
example.predict@0.1.0) as a Python module, built with praenora_add_python_module() and run by the
runtime's provider python.

It does what example-predictor (examples/modules/predictor) does, with the same parameters and
outputs. For each estimate (value x0 at time t0) of its input `state`, it integrates the logistic
equation dx/dt = r * x * (1 - x / K) (r: growth_rate, K: capacity) from t0 with the classical
fixed-step Runge-Kutta method (RK4, step step_size), and emits the value at the times
t_j = window_end + j * output_interval, j = 0 ... floor(horizon / output_interval), where
window_end is the invocation's logical_time. The last step before each t_j is shortened so that
it lands exactly on t_j. Each row carries the estimate's series_id and variable_id, its unit if
it has one, and prediction_kind "latent_mean". Output `predictions`: {"issued_at": window_end,
"time_unit": the state's, "predictions": [...]}, the rows of each estimate in turn.
Parameters (defaults in brackets), all > 0: growth_rate [0.3], capacity [100], horizon [4.0],
step_size [0.05], output_interval [0.5].

The estimates are integrated together, as numpy arrays of 64-bit floats, one element per
estimate: each step updates the estimates that have not reached the output time yet and leaves
the others as they are. Every element goes through the + - * / operations of the C++ module, in
the same order, each rounded once, so the outputs are those of example-predictor, bit for bit.
Nothing depends on the clock or on chance, so the module is deterministic.

diagnostics(): {"predictions", "steps"}, counted since the instance was created: the rows
emitted and the integration steps taken, one per estimate and step. As in example-predictor, which
integrates each estimate before it reads the next, an invocation that fails on an estimate (a
missing field, a time after the window's end) has taken the steps of the estimates before it, and
counts them, but no row; one that fails on the state's time_unit, which is read last, counts its
rows and steps.
"""

import numpy

from praenora import sdk

_NAME = "example-predictor-python"
# A horizon may need at most this many integration steps, and give at most this many rows per
# estimate, so that an invocation stays bounded.
_MAX_STEPS = 1e7
_MAX_ROWS = 1e5
_PARAMETERS = ("growth_rate", "capacity", "horizon", "step_size", "output_interval")


def _invalid(what):
    raise sdk.Error(sdk.Status.INVALID_ARGUMENT, _NAME + ": " + what)


def _number_at(document, key, what):
    """The number `key` of the dict `document` as a float (a JSON number, not a boolean)."""
    value = document.get(key) if isinstance(document, dict) else None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _invalid(what + " has no number " + key)
    return float(value)


def _string_at(document, key, what):
    value = document.get(key) if isinstance(document, dict) else None
    if not isinstance(value, str):
        _invalid(what + " has no string " + key)
    return value


class Predictor(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        values = {"growth_rate": 0.3, "capacity": 100.0, "horizon": 4.0, "step_size": 0.05,
                  "output_interval": 0.5}
        parameters = context.document.get("parameters")
        if isinstance(parameters, dict):
            for key in _PARAMETERS:
                if key in parameters:
                    values[key] = _number_at(parameters, key, "the parameters")
                    if not values[key] > 0.0:
                        _invalid(key + " must be greater than 0")
        self.rate = values["growth_rate"]
        self.capacity = values["capacity"]
        self.horizon = values["horizon"]
        self.step = values["step_size"]
        self.interval = values["output_interval"]
        if self.horizon / self.step > _MAX_STEPS or self.horizon / self.interval > _MAX_ROWS:
            _invalid("the horizon needs too many steps or rows; make step_size or "
                     "output_interval larger")
        self.predictions = 0
        self.steps = 0

    def invoke(self, document):
        window_end = _number_at(document, "logical_time", "the invocation")
        inputs = document.get("inputs")
        if not isinstance(inputs, dict) or "state" not in inputs:
            _invalid("the required input state is missing")
        state = inputs["state"].get("data") if isinstance(inputs["state"], dict) else None
        if not isinstance(state, dict):
            _invalid("input state has no data object")
        estimates = state.get("estimates")
        if not isinstance(estimates, list):
            _invalid("the state has no estimates array")

        # The estimates up to the first one that is invalid, which fails the invocation once
        # those before it are integrated.
        series, variables, units, times, values = [], [], [], [], []
        failure = None
        for estimate in estimates:
            try:
                name = _string_at(estimate, "series_id", "an estimate")
                variable = _string_at(estimate, "variable_id", "an estimate")
                unit = estimate.get("unit")
                time = _number_at(estimate, "time", "an estimate")
                value = _number_at(estimate, "value", "an estimate")
                if time > window_end:
                    _invalid("an estimate at time %f lies after the window's end, %f"
                             % (time, window_end))
            except sdk.Error as error:
                failure = error
                break
            series.append(name)
            variables.append(variable)
            units.append(unit if isinstance(unit, str) else None)
            times.append(time)
            values.append(value)
        time = numpy.array(times, dtype=numpy.float64)
        value = numpy.array(values, dtype=numpy.float64)

        # The output times, and the value of every estimate at each of them.
        targets, columns = [], []
        steps_before = self.steps
        j = 0
        while True:
            offset = j * self.interval
            if offset > self.horizon + self.horizon * 1e-9:
                break  # j > floor(horizon / output_interval), up to rounding
            target = window_end + offset
            time, value = self._advance(time, value, target)
            targets.append(target)
            columns.append(value.tolist())
            j += 1
        if failure is not None:
            raise failure

        rows = []
        for i in range(len(estimates)):
            for target, column in zip(targets, columns):
                row = {"series_id": series[i], "variable_id": variables[i], "time": target,
                       "prediction_kind": "latent_mean", "value": column[i]}
                if units[i] is not None:
                    row["unit"] = units[i]
                rows.append(row)
        self.predictions += len(rows)
        host = self.context.host
        if host.log_enabled(sdk.LogLevel.DEBUG):
            host.log(sdk.LogLevel.DEBUG, "%d predictions for %d estimates (%d integration steps)"
                     % (len(rows), len(estimates), self.steps - steps_before))

        return {"outputs": {"predictions": {"data": {
            "issued_at": window_end,
            "time_unit": _string_at(state, "time_unit", "the state"),
            "predictions": rows,
        }}}}

    def diagnostics(self):
        return {"predictions": self.predictions, "steps": self.steps}

    def _slope(self, x):
        """dx/dt of the logistic equation."""
        return self.rate * x * (1.0 - x / self.capacity)

    def _rk4(self, x, h):
        """One classical Runge-Kutta step of length h (per element) from x."""
        k1 = self._slope(x)
        k2 = self._slope(x + 0.5 * h * k1)
        k3 = self._slope(x + 0.5 * h * k2)
        k4 = self._slope(x + h * k3)
        return x + (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4)

    def _advance(self, time, value, target):
        """Integrates each estimate (time, value) forward to `target` in steps of step_size, the
        last one shortened so that it ends exactly on `target`. (A remainder within a relative 1e-9
        of a whole step is taken as that last step, rather than leaving a step of almost nothing.)
        An estimate that has reached `target` is left as it is."""
        step = self.step
        while True:
            moving = time < target
            count = int(numpy.count_nonzero(moving))
            if count == 0:
                return time, value
            remaining = target - time
            last = remaining <= step + step * 1e-9
            h = numpy.where(last, remaining, step)
            value = numpy.where(moving, self._rk4(value, h), value)
            time = numpy.where(moving, numpy.where(last, target, time + step), time)
            self.steps += count
