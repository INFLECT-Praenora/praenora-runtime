"""The Python side of fx-python-call, a C++ module that calls it through praenora.python@0."""

import numpy


class Stats:
    def __init__(self, document):
        self.calls = 0

    def summary(self, document):
        self.calls += 1
        values = numpy.asarray(document["values"], dtype=numpy.float64)
        return {"count": int(values.size), "sum": float(values.sum()), "calls": self.calls}
