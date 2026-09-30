#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile


query, *modules = sys.argv[1:]
if modules:
    os.execv(query, [query, *modules])

with tempfile.TemporaryDirectory() as empty:
    result = subprocess.run(
        [query], env={**os.environ, "GDK_PIXBUF_MODULEDIR": empty},
    )
sys.exit(result.returncode)
