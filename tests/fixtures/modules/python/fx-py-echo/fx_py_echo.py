"""fx-py-echo: copies the payload of input in to output out ({} when in is absent), with the
members of the parameter "add" (a dict) merged into it. diagnostics() reports how many times
invoke ran."""

from praenora import sdk


class Echo(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.add = context.document["parameters"].get("add", {})
        self.invocations = 0

    def invoke(self, document):
        self.invocations += 1
        source = document["inputs"].get("in")
        data = dict(source["data"]) if source is not None else {}
        data.update(self.add)
        return {"outputs": {"out": {"data": data}}}

    def diagnostics(self):
        return {"invocations": self.invocations}
