"""fx-py-large: outputs the numbers 0, 1, ..., count - 1 (the parameter count), a large result
document."""

from praenora import sdk


class Large(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.count = context.document["parameters"]["count"]

    def invoke(self, document):
        return {"outputs": {"out": {"data": {"numbers": list(range(self.count))}}}}
