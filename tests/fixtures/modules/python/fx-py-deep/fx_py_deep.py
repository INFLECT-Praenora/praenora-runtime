"""fx-py-deep: each invoke recurses depth calls deep (the parameter depth, default 900, close to
Python's default recursion limit of 1000) through functools.lru_cache, whose wrapper enters the
interpreter again for each call, so that each level takes the C stack of the calling thread; the
output is the depth reached."""

import functools

from praenora import sdk


@functools.lru_cache(maxsize=None)
def _depth(n):
    return 0 if n == 0 else 1 + _depth(n - 1)


class Deep(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.depth = context.document["parameters"].get("depth", 900)

    def invoke(self, document):
        _depth.cache_clear()
        return {"outputs": {"out": {"data": {"depth": _depth(self.depth)}}}}
