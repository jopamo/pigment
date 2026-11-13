#!/usr/bin/env python3

import os
import signal
import shutil
import stat
import sys
import subprocess
import tempfile
import threading
import time


def query_cache(query, output, diagnostics, timeout=120):
    errors = []
    deadline = time.monotonic() + timeout
    with subprocess.Popen([query], stdout=subprocess.PIPE, stderr=diagnostics,
                          start_new_session=True) as process:
        def terminate():
            if hasattr(os, "killpg"):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            else:
                process.kill()

        def copy_output():
            try:
                shutil.copyfileobj(process.stdout, output, length=65536)
            except Exception as error:
                errors.append(error)
                terminate()

        writer = threading.Thread(target=copy_output)
        started = False
        try:
            writer.start()
            started = True
            code = process.wait(timeout=max(0, deadline - time.monotonic()))
            writer.join(timeout=max(0, deadline - time.monotonic()))
            if writer.is_alive():
                raise subprocess.TimeoutExpired([query], timeout)
            if errors:
                raise errors[0]
            return code
        finally:
            if process.poll() is None or writer.is_alive():
                terminate()
            process.wait()
            if started:
                writer.join()


def update_cache(bindir, libdir, binary_version):
    query_loaders = os.path.join(bindir, "gdk-pixbuf-query-loaders")
    loaders_dir = os.path.join(libdir, "gdk-pixbuf-2.0", binary_version)
    loaders_cache = os.path.join(loaders_dir, "loaders.cache")
    os.makedirs(loaders_dir, exist_ok=True)
    candidate = None
    try:
        with tempfile.TemporaryFile() as diagnostics, \
                tempfile.NamedTemporaryFile(dir=loaders_dir, prefix=".loaders.cache-",
                                             delete=False) as output:
            candidate = output.name
            code = query_cache(query_loaders, output, diagnostics)
            diagnostics.seek(0)
            message = diagnostics.read(2000).decode("utf-8", errors="replace")
            if code or message:
                raise ValueError(f"{query_loaders} exited with status "
                                 f"{code}: {message.strip()}")
            output.flush()
            try:
                previous = os.stat(loaders_cache)
            except FileNotFoundError:
                previous = None
            if previous:
                os.chmod(candidate, stat.S_IMODE(previous.st_mode))
                if hasattr(os, "chown"):
                    current = os.stat(candidate)
                    if (current.st_uid, current.st_gid) != (previous.st_uid, previous.st_gid):
                        os.chown(candidate, previous.st_uid, previous.st_gid)
            else:
                mask = os.umask(0o077)
                os.umask(mask)
                os.chmod(candidate, 0o666 & ~mask)
        os.replace(candidate, loaders_cache)
        candidate = None
    finally:
        if candidate is not None:
            try:
                os.unlink(candidate)
            except FileNotFoundError:
                pass


def main():
    if 'DESTDIR' in os.environ:
        print("*** Warning: loaders.cache not built because DESTDIR is set")
        print("***          You will need to manually call gdk-pixbuf-query-loaders")
        return
    update_cache(*sys.argv[1:])


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f"loader cache update failed: {error}")
    except KeyboardInterrupt:
        sys.exit(130)
