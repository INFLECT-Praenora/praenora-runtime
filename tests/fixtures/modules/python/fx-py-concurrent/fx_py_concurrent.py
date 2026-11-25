"""fx-py-concurrent: a module whose instances may be called at the same time (its manifest
declares features.concurrency "instance"). With the parameter barrier, each invoke first waits at a
barrier of two that every instance of the process shares, so it returns only when two instances
are in invoke at once (a BrokenBarrierError after 30 s otherwise). The output is the input's value
times the parameter factor (default 1) and the step's name."""

import threading

from praenora import sdk

_BARRIER = threading.Barrier(2)


class Concurrent(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        parameters = context.document["parameters"]
        self.barrier = parameters.get("barrier", False)
        self.factor = parameters.get("factor", 1)

    def invoke(self, document):
        if self.barrier:
            _BARRIER.wait(timeout=30)
        source = document["inputs"].get("in")
        value = source["data"].get("value", 0) if source is not None else 0
        return {"outputs": {"out": {"data": {"value": value * self.factor,
                                             "step": document["step"]}}}}
