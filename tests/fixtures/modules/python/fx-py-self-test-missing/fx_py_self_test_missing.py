"""fx-py-self-test-missing: an echo built with SELF_TEST whose class defines no self_test."""

from praenora import sdk


class NoSelfTest(sdk.Module):
    def invoke(self, document):
        source = document["inputs"].get("in")
        return {"outputs": {"out": {"data": source["data"] if source is not None else {}}}}
