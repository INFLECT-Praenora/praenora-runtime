"""The object the runtime creates for a Python module.

The runtime creates it through the class, with the class method Module._praenora_adapter(args),
which calls adapter_of(cls, args) with args {"create": <create document>}; create(args) does the
same for {"class": "package.module:Name", "create": <create document>}, importing the class
itself. The runtime then calls the methods of the object it gets, each with a JSON-able document
and returning a JSON-able value. Without "create" (absent or null) the adapter is class-only: it
does not construct the class, and only self_test() works. The ABI's self_test has no instance, so
the runtime asks a class-only adapter, which calls the class method self_test(host).
"""

import importlib

from . import CreateContext, Error, Host, Module, Status, _runtime


class Adapter:
    """Holds a module class, its host and, unless class-only, one instance."""

    def __init__(self, cls, host, instance):
        self._cls = cls
        self._host = host
        self._instance = instance

    @property
    def module(self):
        """The instance of the class, or None for a class-only adapter."""
        return self._instance

    def _module(self, call):
        if self._instance is None:
            raise Error(Status.INVALID_STATE,
                        "%s: the adapter of %s was created for self_test only"
                        % (call, _qualified_name(self._cls)))
        return self._instance

    def initialize(self, document):
        self._module("initialize").initialize(document)
        return None

    def invoke(self, document):
        return self._module("invoke").invoke(document)

    def finalize(self, document=None):
        self._module("finalize").finalize()
        return None

    def diagnostics(self, document=None):
        result = self._module("diagnostics").diagnostics()
        return {} if result is None else result

    def self_test(self, document=None):
        method = getattr(self._cls, "self_test", None)
        if method is None:
            raise Error(Status.UNSUPPORTED,
                        "the class %s defines no self_test" % _qualified_name(self._cls))
        return method(self._host)


def _qualified_name(cls):
    return "%s:%s" % (cls.__module__, cls.__qualname__)


def _load_class(spec):
    module_name, separator, attribute = str(spec).partition(":")
    if not separator or not module_name or not attribute:
        raise ValueError("the class %r is not written package.module:Name" % (spec,))
    value = importlib.import_module(module_name)
    for part in attribute.split("."):
        value = getattr(value, part)
    return value


def adapter_of(cls, args, host=None):
    """Creates the adapter of the module class cls.

    With args["create"] (the create document) the class is constructed; without it (absent or
    null, or args null) the adapter is class-only and serves self_test() alone. host is the Host
    the module gets: the runtime's (the core's backend, or a null one outside the core) unless a
    test kit gives its own.
    """
    if not (isinstance(cls, type) and issubclass(cls, Module)):
        raise TypeError("the class %r is not a subclass of praenora.sdk.Module" % (cls,))
    if host is None:
        host = Host(_runtime.backend())
    document = None if args is None else args.get("create")
    if document is None:
        return Adapter(cls, host, None)
    return Adapter(cls, host, cls(CreateContext(host, document)))


def create(args):
    """Creates the adapter of args["class"] ("package.module:Name"), as adapter_of() does."""
    cls = _load_class(args["class"])
    if not (isinstance(cls, type) and issubclass(cls, Module)):
        raise TypeError("the class %s is not a subclass of praenora.sdk.Module" % args["class"])
    return adapter_of(cls, args)
