"""fx-py-cancel: invoke polls the host's cancel flag every 5 ms, for at most the parameter limit_s
seconds (default 60), and raises sdk.Cancelled as soon as the flag is raised."""

import time

from praenora import sdk


class Cancel(sdk.Module):
    def __init__(self, context):
        super().__init__(context)
        self.limit_s = context.document["parameters"].get("limit_s", 60)

    def invoke(self, document):
        end = time.monotonic() + self.limit_s
        polls = 0
        while time.monotonic() < end:
            polls += 1
            if self.context.host.cancel_requested():
                raise sdk.Cancelled("fx-py-cancel: cancelled after %d polls" % polls,
                                    {"polls": polls})
            time.sleep(0.005)
        return {"outputs": {"out": {"data": {"polls": polls}}}}
