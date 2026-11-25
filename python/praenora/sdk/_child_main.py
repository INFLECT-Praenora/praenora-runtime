"""The main module of child processes that multiprocessing starts for a module or provider.

The runtime's Python core makes multiprocessing import this module as the children's __main__.
Importing it points file descriptor 1 at file descriptor 2 and sys.stdout at sys.stderr, so what
a child prints appears on the standard error of the runtime's process and never on its standard
output, which carries the documents of the tool's --json output. Importing it in any other process
redirects that process's output the same way: import it nowhere else.
"""

import os
import sys

try:
    sys.stdout.flush()
except (AttributeError, ValueError, OSError):
    pass
try:
    os.dup2(2, 1)
except OSError:
    pass
if getattr(sys, "stderr", None) is not None:
    sys.stdout = sys.stderr
