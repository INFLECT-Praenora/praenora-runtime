"""fx-py-abort: invoke ends its process with os.abort(). For module hosts only."""

import os

from praenora import sdk


class Abort(sdk.Module):
    def invoke(self, document):
        os.abort()
