"""fx-py-roadrunner: simulates a small Antimony model with the environment's roadrunner package, in
initialize: x' = -k x, with x(0) = 10 and k = 0.5, from 0 to 2 at five points, with tolerances of
1e-12. Each invoke outputs the payload of input in ({} when absent) as "in" and the rows of that
simulation, [time, x], as "rows"."""

import antimony
import roadrunner

from praenora import sdk

_MODEL = """
model decay
  compartment cell = 1
  species x in cell
  J0: x -> ; k * x
  x = 10
  k = 0.5
end
"""


class Roadrunner(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.rows = None

    def initialize(self, document):
        if antimony.loadAntimonyString(_MODEL) < 0:
            raise sdk.Error(sdk.Status.DATA_INVALID, antimony.getLastError())
        simulator = roadrunner.RoadRunner(antimony.getSBMLString(antimony.getMainModuleName()))
        simulator.integrator.relative_tolerance = 1e-12
        simulator.integrator.absolute_tolerance = 1e-12
        simulator.timeCourseSelections = ["time", "x"]
        self.rows = simulator.simulate(0, 2, 5).tolist()

    def invoke(self, document):
        source = document["inputs"].get("in")
        data = {"in": source["data"] if source is not None else {}, "rows": self.rows}
        return {"outputs": {"out": {"data": data}}}
