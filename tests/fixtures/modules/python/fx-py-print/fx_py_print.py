"""fx-py-print: each invoke prints a line to sys.stdout and one to sys.stderr, and logs a warning
through logging, each naming the tick; the output is the tick."""

import logging
import sys

from praenora import sdk

_LOGGER = logging.getLogger("fx_py_print")


class Print(sdk.Module):
    def invoke(self, document):
        tick = document["tick"]
        print("fx-py-print: printed at tick", tick)
        print("fx-py-print: to stderr at tick", tick, file=sys.stderr)
        _LOGGER.warning("fx-py-print: logged at tick %d", tick)
        return {"outputs": {"out": {"data": {"tick": tick}}}}
