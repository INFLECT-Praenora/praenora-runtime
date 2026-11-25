"""fx-py-plain-class: names a class that does not derive from praenora.sdk.Module, which cannot be
created."""


class Plain:
    def __init__(self, context):
        self.context = context

    def invoke(self, document):
        return {"outputs": {"out": {"data": {}}}}
