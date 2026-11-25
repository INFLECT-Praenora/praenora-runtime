"""Output: prints, logs and writes text that is not plain ASCII, on the calling thread and on a
thread of its own."""

import logging
import sys
import threading

_LOGGER = logging.getLogger("core_fixtures.output")


class Output:
    def __init__(self, document):
        print("constructed")
        self._event = threading.Event()
        self._thread = None

    def prints(self, document):
        print("printed:", document["text"])
        print("to stderr:", document["text"], file=sys.stderr)
        print("unfinished", end="")
        return None

    def logs(self, document):
        _LOGGER.debug("a debug record")
        _LOGGER.info("an info record")
        _LOGGER.warning("a warning record")
        _LOGGER.error("an error record")
        _LOGGER.critical("a critical record")
        return None

    def text_that_is_not_utf8(self, document):
        text = "caf\u00e9 \udcff"
        print("printed", text)
        _LOGGER.warning("logged %s", text)
        sys.stdout.buffer.write(b"bytes \xff\n")
        raise ValueError(text)

    def partial_bytes(self, document):
        sys.stdout.buffer.write(b"partial \xc3")  # the call never ends the sequence
        return None

    def background(self, document):
        def wait_then_print():
            self._event.wait()
            print("printed by a thread of its own")

        self._thread = threading.Thread(target=wait_then_print)
        self._thread.start()
        return None

    def finish(self, document):
        self._event.set()
        self._thread.join()
        return None
