"""Isolated lifecycle wrapper for the real Fcitx host and real librime worker.

No compositor is started: this intentionally tests standard-candidate fallback,
not KDE/Wayland placement or rendering.
"""
import os
import json
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

host, worker, addon, source = map(pathlib.Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="r-ime-fcitx-host-") as temporary, \
     tempfile.TemporaryDirectory(prefix="r-ime-host-executables-", dir=os.environ.get("RIME_TEST_EXEC_TMPDIR", "/tmp")) as executables:
    root = pathlib.Path(temporary)
    executable_root = pathlib.Path(executables)
    # Stage the exact built binaries on the Linux filesystem, as a real install
    # does. Executing demand-paged ELF code from /mnt (Windows filesystem) adds
    # unrelated host I/O stalls to the addon's bounded IPC deadline.
    (executable_root / "bin").mkdir()
    (executable_root / "modules").mkdir()
    local_host, local_worker, local_addon = executable_root / "bin/host", executable_root / "bin/r-ime-engine", executable_root / "modules/r-ime.so"
    shutil.copy2(host, local_host)
    shutil.copy2(worker, local_worker)
    shutil.copy2(addon, local_addon)
    host, worker, addon = local_host, local_worker, local_addon
    print(f"Fcitx host test user data: {root.parent}; executable staging: {executable_root.parent}", flush=True)
    shutil.copytree(source / "data/rime", root / "schemas")
    data = root / "fcitx-data"
    (data / "addon").mkdir(parents=True)
    (data / "inputmethod").mkdir()
    shutil.copyfile(source / "src/fcitx/r-ime-addon.conf", data / "addon/r-ime.conf")
    shutil.copyfile(source / "src/fcitx/r-ime.conf", data / "inputmethod/r-ime.conf")
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root / "runtime"),
               XDG_CONFIG_HOME=str(root / "config"), XDG_DATA_HOME=str(root / "data"),
               XDG_CACHE_HOME=str(root / "cache"), HOME=str(root / "home"))
    pathlib.Path(env["XDG_RUNTIME_DIR"]).mkdir(mode=0o700)
    pathlib.Path(env["HOME"]).mkdir()
    # The addon may try to launch its worker during construction. A worker is
    # already listening; a second process exits on the singleton lock.
    env["PATH"] = str(worker.parent) + os.pathsep + env.get("PATH", "")
    with (root / "engine.log").open("w+") as log:
        engine = subprocess.Popen([str(worker), "--overlay", str(root / "schemas")],
                                  env=env, cwd=root, stdout=log, stderr=log, start_new_session=True)
        try:
            deadline = time.monotonic() + 90
            address = pathlib.Path(env["XDG_RUNTIME_DIR"]) / "r-ime/engine.sock"
            while not address.exists():
                if engine.poll() is not None:
                    raise RuntimeError("engine exited before creating its socket")
                if time.monotonic() >= deadline:
                    raise TimeoutError("engine deployment exceeded 90 seconds")
                time.sleep(.05)
            with socket.socket(socket.AF_UNIX) as probe:
                probe.settimeout(5)
                probe.connect(str(address))
                stream = probe.makefile("rwb", buffering=0)
                for sequence, operation in enumerate(("status",), 1):
                    start = time.monotonic()
                    stream.write((json.dumps(dict(version=1, id=sequence, session=0, op=operation))+"\n").encode())
                    reply = json.loads(stream.readline())
                    print(f"Engine readiness {operation}: {(time.monotonic()-start)*1000:.2f} ms, ok={reply['ok']}", flush=True)
                stream.close()
            child = subprocess.Popen([str(host), str(data), str(addon.parent)],
                                     env=env, cwd=root, start_new_session=True)
            try:
                returncode = child.wait(timeout=25)
            finally:
                # Also reap worker-launch attempts made by the addon, even if
                # its host exits while one is still loading shared libraries.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
            if returncode:
                log.seek(0)
                print(log.read(), file=sys.stderr)
                diagnostic = pathlib.Path(env["XDG_CACHE_HOME"]) / "r-ime/diagnostic.json"
                if diagnostic.exists():
                    print("Addon diagnostic:", diagnostic.read_text(), file=sys.stderr)
            sys.exit(returncode)
        except BaseException:
            if sys.exc_info()[0] is not SystemExit:
                log.seek(0)
                print(log.read(), file=sys.stderr)
            raise
        finally:
            try:
                os.killpg(engine.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                engine.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(engine.pid, signal.SIGKILL)
                engine.wait()
