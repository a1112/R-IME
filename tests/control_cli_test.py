"""Exercise the public CLI against an isolated real worker, on Linux or Windows."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    engine, ctl, overlay = map(Path, sys.argv[1:4])
    assert ctl.is_file(), "r-ime-ctl has not been implemented"
    with tempfile.TemporaryDirectory(prefix="r-ime-control-中文-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        if os.name == "nt":
            env["RIME_USER_ROOT"] = str(root)
            command = [str(engine)]
        else:
            for key, name in [("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                              ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime")]:
                env[key] = str(root / name)
                (root / name).mkdir(mode=0o700)
            command = [str(engine), "--overlay", str(overlay)]
        log = (root / "worker.log").open("wb")
        worker = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        bridges = []

        def run(*args, input=None, code=0):
            result = subprocess.run([str(ctl), *args], input=input, env=env,
                                    capture_output=True, text=True, encoding="utf-8", timeout=200)
            assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
            assert not result.stderr, (args, result.stderr)
            return json.loads(result.stdout)

        def bridge():
            process = subprocess.Popen([str(ctl), "rpc"], env=env, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, encoding="utf-8", bufsize=1)
            bridges.append(process)
            return process

        def request(process, document):
            process.stdin.write(json.dumps(document, ensure_ascii=False) + "\n")
            process.stdin.flush()
            reply = json.loads(process.stdout.readline())
            return reply

        try:
            help_result = subprocess.run([str(ctl), "--help"], env=env, capture_output=True,
                                         text=True, encoding="utf-8", timeout=10)
            assert help_result.returncode == 0 and "rpc" in help_result.stdout
            assert run("bad-command", code=2)["error"]["code"] == "invalid_arguments"
            assert run("--timeout", "-1", "status", code=2)["ok"] is False
            for _ in range(600):
                assert worker.poll() is None, (root / "worker.log").read_text(errors="replace")
                result = subprocess.run([str(ctl), "status"], env=env, capture_output=True,
                                        text=True, encoding="utf-8", timeout=5)
                if result.returncode == 0:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("private worker did not become ready")
            status = json.loads(result.stdout)
            assert status["ok"] and status["config"]["input"]["schema"] == "rime_ime_pinyin"
            assert run("config", "get")["config"] == status["config"]

            a, b = bridge(), bridge()
            session = request(a, {"op": "create"})["session"]
            for key in "nihao":
                reply = request(a, {"op": "key", "session": session, "keycode": ord(key),
                                    "modifiers": 0})
            assert reply["candidates"][0]["text"] == "你好"
            denied = request(b, {"op": "status", "session": session})
            assert denied["ok"] is False and denied["error"]["code"] == "engine_error"
            assert request(a, {"op": "select", "session": session, "index": 0})["commit"] == "你好"
            assert request(a, {"op": "destroy", "session": session})["ok"]

            # One rejected line must not close or desynchronize the bridge.
            a.stdin.write("not-json\n")
            a.stdin.flush()
            assert json.loads(a.stdout.readline())["error"]["code"] == "invalid_request"
            assert request(a, {"op": "status"})["ok"]
            for bad in [[], {}, {"op": 1}, {"op": "status", "version": 2},
                        {"op": "status", "session": -1}]:
                assert not request(a, bad)["ok"]

            snapshot = request(a, {"op": "configuration"})
            session = request(a, {"op": "create"})["session"]
            request(a, {"op": "key", "session": session, "keycode": ord("n"), "modifiers": 0})
            assert run("config", "set", "/appearance/theme", '"dark"')["config"]["appearance"]["theme"] == "dark"
            stale = request(b, {"op": "configure", "config": snapshot["config"],
                                "expected_config": snapshot["config"],
                                "expected_phrases": snapshot["phrases"]})
            assert not stale["ok"] and stale["error"]["code"] == "conflict", stale
            assert stale["sessions_invalidated"] is False
            assert request(a, {"op": "status", "session": session})["preedit"]
            request(a, {"op": "reset", "session": session})
            request(a, {"op": "destroy", "session": session})
            before = run("config", "get")["config"]
            assert not run("config", "set", "/appearance/page_size", "99", code=1)["ok"]
            assert not run("config", "set", "/general/unknown", "true", code=1)["ok"]
            assert run("config", "get")["config"] == before
            assert run("mode", "ascii")["config"]["general"]["ascii"]
            assert request(a, {"op": "create"})["ascii"]
            run("mode", "chinese")
            run("schema", "flypy")
            assert request(a, {"op": "create"})["schema"] == "rime_ime_flypy"
            run("schema", "pinyin")

            assert run("phrases", "list")["phrases"] == []
            run("phrases", "set", "ceshi", "中文控制短语", "123")
            run("phrases", "set", "ceshi", "中文控制短语", "456")
            phrases = run("phrases", "list")["phrases"]
            assert phrases == [{"code": "ceshi", "text": "中文控制短语", "weight": 456}]
            session = request(a, {"op": "create"})["session"]
            for key in "ceshi":
                reply = request(a, {"op": "key", "session": session, "keycode": ord(key),
                                    "modifiers": 0})
            assert reply["candidates"][0]["text"] == "中文控制短语"
            export = root / "短语 export.tsv"
            run("phrases", "export", str(export))
            assert export.read_text(encoding="utf-8") == "ceshi\t中文控制短语\t456\n"
            archive = root / "备份 backup.json"
            run("backup", str(archive))
            assert json.loads(archive.read_text(encoding="utf-8"))["format"] == "r-ime-backup"
            blocked_export = root / "directory instead of file"
            blocked_export.mkdir()
            export_error = run("backup", str(blocked_export), code=1)
            assert export_error["error"]["code"] == "io_error"
            assert export_error["sessions_invalidated"] is True
            run("phrases", "remove", "ceshi", "中文控制短语")
            assert run("phrases", "list")["phrases"] == []
            run("restore", str(archive))
            assert run("phrases", "list")["phrases"] == phrases
            run("phrases", "import", str(export))
            export.write_text("invalid row\n", encoding="utf-8")
            assert not run("phrases", "import", str(export), code=1)["ok"]
            assert run("phrases", "list")["phrases"] == phrases
            config_file = root / "config import.json"
            before["appearance"]["theme"] = "light"
            config_file.write_text(json.dumps(before), encoding="utf-8")
            assert run("config", "import", str(config_file))["config"] == before
            if os.name == "nt":
                assert request(a, {"op": "shutdown"})["ok"]
                worker.wait(timeout=10)
            else:
                worker.terminate()
                worker.wait(timeout=10)
            disconnected = run("status", code=1)
            assert disconnected["error"]["code"] == "transport_error"
            assert disconnected["sessions_invalidated"] is True
            print("Local control CLI: real sessions, ownership, JSONL errors, config, modes, Unicode phrases and backup passed")
        finally:
            for process in bridges:
                if process.poll() is None:
                    process.stdin.close()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                process.stdout.close()
                process.stderr.close()
            if worker.poll() is None:
                worker.terminate()
                try:
                    worker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait()
            log.close()


if __name__ == "__main__":
    main()
