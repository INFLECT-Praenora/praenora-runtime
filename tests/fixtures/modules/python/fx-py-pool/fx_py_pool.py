"""fx-py-pool: a spawn pool of two processes, opened in initialize and closed in finalize. Each
invoke maps a numpy computation (the sum of the square roots of 0, ..., n - 1) over the parameter
counts in the pool's children and outputs the sums and the default start method. The parameter
report_children first logs the children's process ids ("fx-py-pool children: <pid> <pid>");
child_print makes a child print that text; sleep_s then sleeps that long without polling the
cancel flag. report_executables adds "executables", the interpreters that the children ran
(sys.executable, each once, sorted)."""

import multiprocessing
import os
import sys
import time

from praenora import sdk


def _square_root_sum(count):
    import numpy

    return float(numpy.sqrt(numpy.arange(count, dtype=numpy.float64)).sum())


def _print_text(text):
    print(text, flush=True)
    return os.getpid()


def _executable(_):
    return sys.executable


class Pool(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        parameters = context.document["parameters"]
        self.counts = parameters.get("counts", [10, 1000, 100000])
        self.child_print = parameters.get("child_print")
        self.report_children = parameters.get("report_children", False)
        self.report_executables = parameters.get("report_executables", False)
        self.sleep_s = parameters.get("sleep_s", 0)
        self.pool = None

    def initialize(self, document):
        self.pool = multiprocessing.get_context("spawn").Pool(2)

    def invoke(self, document):
        if self.report_children:
            # The pool's children exist from initialize on: their ids are known before any work.
            children = sorted(child.pid for child in multiprocessing.active_children())
            self.context.host.log(sdk.LogLevel.INFO,
                                  "fx-py-pool children: " + " ".join(map(str, children)))
        sums = self.pool.map(_square_root_sum, self.counts)
        if self.child_print is not None:
            self.pool.map(_print_text, [self.child_print])
        if self.sleep_s:
            time.sleep(self.sleep_s)
        data = {"sums": sums, "start_method": multiprocessing.get_start_method()}
        if self.report_executables:
            data["executables"] = sorted(set(self.pool.map(_executable, range(4))))
        return {"outputs": {"out": {"data": data}}}

    def finalize(self):
        # The runtime's process never finalizes its interpreter, so nothing else would stop the
        # pool's children.
        if self.pool is not None:
            self.pool.close()
            self.pool.join()
            self.pool = None
