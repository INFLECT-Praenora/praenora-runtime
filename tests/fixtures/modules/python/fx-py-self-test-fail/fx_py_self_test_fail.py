"""fx-py-self-test-fail: an echo whose self test reports a failure."""

from praenora import sdk


class SelfTestFail(sdk.Module):
    def invoke(self, document):
        source = document["inputs"].get("in")
        return {"outputs": {"out": {"data": source["data"] if source is not None else {}}}}

    @classmethod
    def self_test(cls, host):
        return {"passed": False,
                "checks": [{"name": "tables", "passed": False, "message": "wrong value"}]}
