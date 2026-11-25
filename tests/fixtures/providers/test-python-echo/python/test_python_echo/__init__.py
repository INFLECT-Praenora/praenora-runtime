"""test_python_echo: the Python side of the fixture provider test-python-echo, which offers the test
service test.echo@0 (tests/support/test_echo.h) through this class.

The provider's start() creates one Echo for the process, with the document null; its table's echo
calls Echo.echo with {"input": <the input's bytes in hexadecimal>} and writes the bytes of the
hexadecimal text it returns. The record "echoed <n> bytes" (level debug) goes to the host of the
call: the calling module's when the service function was given one, the provider's otherwise.
"""

from praenora import sdk
from praenora.sdk import _runtime


class Echo:
    def __init__(self, document):
        self.host = sdk.Host(_runtime.backend())

    def echo(self, document):
        data = bytes.fromhex(document["input"])
        self.host.log(sdk.LogLevel.DEBUG, "echoed %d bytes" % len(data))
        return data.hex()
