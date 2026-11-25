"""fx-py-fail: every invoke raises a ValueError, two calls deep, so that the failure's traceback
has several frames."""

from praenora import sdk


def _refuse(value):
    raise ValueError("fx-py-fail: the tick %r is refused" % (value,))


def _check(document):
    _refuse(document["tick"])


class Fail(sdk.Module):
    def invoke(self, document):
        _check(document)
        return {"outputs": {"out": {"data": {}}}}
