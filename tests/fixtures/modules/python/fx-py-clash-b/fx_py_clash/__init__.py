"""fx_py_clash as fx-py-clash-b ships it: fx-py-clash-a and fx-py-clash-b ship two packages of
one name. The output names the module that ran."""

from praenora import sdk


class Echo(sdk.Module):
    def invoke(self, document):
        return {"outputs": {"out": {"data": {"from": "b"}}}}
