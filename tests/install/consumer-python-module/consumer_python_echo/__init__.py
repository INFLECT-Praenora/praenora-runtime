"""consumer-python-echo: a Python module built outside praenora-runtime against the installed
module SDK only (praenora_add_python_module()). It implements the test capability test.echo@0.1.0:
each invocation copies the payload of input in to output out ({} when in is absent) and adds
"echoed_by": "consumer-python-echo". The install test runs it with the installed tool."""

from praenora import sdk


class Echo(sdk.Module):
    def invoke(self, document):
        source = document["inputs"].get("in")
        data = dict(source["data"]) if source is not None else {}
        data["echoed_by"] = "consumer-python-echo"
        return {"outputs": {"out": {"data": data}}}
