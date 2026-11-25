"""fx-py-self-test: an echo with a self test that passes."""

from praenora import sdk


class SelfTest(sdk.Module):
    def invoke(self, document):
        source = document["inputs"].get("in")
        return {"outputs": {"out": {"data": source["data"] if source is not None else {}}}}

    @classmethod
    def self_test(cls, host):
        host.log(sdk.LogLevel.INFO, "fx-py-self-test: checking")
        return {"passed": True,
                "checks": [{"name": "arithmetic", "passed": 1 + 1 == 2, "message": "1 + 1 == 2"}]}
