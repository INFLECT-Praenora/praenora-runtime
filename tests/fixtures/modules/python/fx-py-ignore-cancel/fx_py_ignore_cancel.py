"""fx-py-ignore-cancel: invoke runs Python code without a pause for the parameter seconds (default
3600), and never polls the host's cancel flag."""

import time

from praenora import sdk


class IgnoreCancel(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.seconds = context.document["parameters"].get("seconds", 3600)

    def invoke(self, document):
        end = time.monotonic() + self.seconds
        spins = 0
        while time.monotonic() < end:
            spins += 1
        return {"outputs": {"out": {"data": {"spins": spins}}}}
