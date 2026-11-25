"""Poller: polls the host's cancel flag, as module code does in a long loop."""

from praenora import sdk
from praenora.sdk import _runtime


class Poller:
    def __init__(self, document):
        self.host = sdk.Host(_runtime.backend())

    def poll(self, document):
        for polls in range(1, document["limit"] + 1):
            if self.host.cancel_requested():
                raise sdk.Cancelled(details={"polls": polls})
        return {"polls": document["limit"]}
