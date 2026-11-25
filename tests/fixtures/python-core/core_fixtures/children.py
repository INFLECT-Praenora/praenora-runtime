"""Children: work done in child processes of a spawn pool."""

import multiprocessing
import sys


def _square_root_sum(count):
    import numpy

    return {
        "value": float(numpy.sqrt(numpy.arange(count, dtype=numpy.float64)).sum()),
        "executable": sys.executable,
        "isolated": sys.flags.isolated,
    }


def _print_text(text):
    print(text, flush=True)
    return True


def _sdk_facts(_):
    import praenora.sdk

    main = sys.modules["__main__"]
    return {
        "sdk_file": praenora.sdk.__file__,
        "status": int(praenora.sdk.Status.DATA_INVALID),
        "main": getattr(getattr(main, "__spec__", None), "name", None),
        "stdout_is_stderr": sys.stdout is sys.stderr,
    }


def _map(function, values, processes):
    pool = multiprocessing.get_context("spawn").Pool(processes)
    try:
        return pool.map(function, values)
    finally:
        pool.close()
        pool.join()


class Children:
    def __init__(self, document):
        pass

    def square_root_sums(self, document):
        return _map(_square_root_sum, document["counts"], 2)

    def print_text(self, document):
        return _map(_print_text, [document["text"]], 1)

    def sdk_facts(self, document):
        return _map(_sdk_facts, [None], 1)
