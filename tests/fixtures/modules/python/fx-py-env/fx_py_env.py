"""fx-py-env: outputs the value of PYTHONHASHSEED in its process's environment (null when unset)."""

import os

from praenora import sdk


class Env(sdk.Module):
    def invoke(self, document):
        return {"outputs": {"out": {"data": {"PYTHONHASHSEED": os.environ.get("PYTHONHASHSEED")}}}}
