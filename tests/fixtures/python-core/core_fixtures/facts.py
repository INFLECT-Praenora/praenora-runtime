"""Facts: what the interpreter of the core says about itself, and cached bytecode written beside a
source, for the test that the interpreter does not read it."""

import importlib.util
import marshal
import multiprocessing
import os
import sys


class Facts:
    def __init__(self, document):
        pass

    def interpreter(self, document):
        main = sys.modules["__main__"]
        return {
            "hash_randomization": sys.flags.hash_randomization,
            "hash": hash("abc"),
            "start_method": multiprocessing.get_start_method(),
            "isolated": sys.flags.isolated,
            "dont_write_bytecode": sys.flags.dont_write_bytecode,
            "utf8_mode": sys.flags.utf8_mode,
            "executable": sys.executable,
            "path": list(sys.path),
            "main_spec": getattr(getattr(main, "__spec__", None), "name", None),
            "pycache_prefix": sys.pycache_prefix,
        }

    def write_cached_bytecode(self, document):
        """Writes, in the __pycache__ directory beside document["source"], the bytecode of the text
        document["cached"] under the source's own modification time and size (the pyc format of
        PEP 552), as a stale or foreign cache file would be; returns its path."""
        source = document["source"]
        code = compile(document["cached"], source, "exec")
        status = os.stat(source)
        data = (importlib.util.MAGIC_NUMBER + (0).to_bytes(4, "little")
                + (int(status.st_mtime) & 0xFFFFFFFF).to_bytes(4, "little")
                + (status.st_size & 0xFFFFFFFF).to_bytes(4, "little") + marshal.dumps(code))
        directory, name = os.path.split(source)
        cache = os.path.join(directory, "__pycache__", "%s.%s.pyc"
                             % (os.path.splitext(name)[0], sys.implementation.cache_tag))
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, "wb") as out:
            out.write(data)
        return cache
