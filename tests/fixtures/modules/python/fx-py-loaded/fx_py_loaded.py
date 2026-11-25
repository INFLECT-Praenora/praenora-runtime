"""fx-py-loaded: outputs the payload of input in ({} when absent) as "in" and, as "loaded", whether
a module of each name of the parameter "names" is loaded in its interpreter (sys.modules)."""

import sys

from praenora import sdk


class Loaded(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.names = list(context.document["parameters"].get("names", []))

    def invoke(self, document):
        source = document["inputs"].get("in")
        data = {
            "in": source["data"] if source is not None else {},
            "loaded": {name: name in sys.modules for name in self.names},
        }
        return {"outputs": {"out": {"data": data}}}
