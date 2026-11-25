"""fx-py-numpy: computes with numpy. Each invoke gives the sum and the sum of the square roots of
0, 1, ..., count - 1 (the parameter count, default 1000), and one draw of a normal generator that
initialize seeds with the run's seed."""

import numpy

from praenora import sdk


class Numpy(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.count = context.document["parameters"].get("count", 1000)
        self.generator = None

    def initialize(self, document):
        # The module's randomness comes from the run's seed, so a run is reproducible.
        self.generator = numpy.random.default_rng(document["run"]["seed"])

    def invoke(self, document):
        values = numpy.arange(self.count, dtype=numpy.float64)
        return {"outputs": {"out": {"data": {
            "sum": float(values.sum()),
            "square_root_sum": float(numpy.sqrt(values).sum()),
            "draw": float(self.generator.standard_normal()),
        }}}}
