"""Use: python control_client.py /absolute/path/to/r-ime-ctl[.exe]."""
import json
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: control_client.py /absolute/path/to/r-ime-ctl[.exe]")
    with subprocess.Popen(
        [sys.argv[1], "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        text=True, encoding="utf-8", bufsize=1,
    ) as bridge:
        def call(op, **fields):
            bridge.stdin.write(json.dumps({"op": op, **fields}, ensure_ascii=False) + "\n")
            bridge.stdin.flush()
            line = bridge.stdout.readline()
            if not line:
                raise RuntimeError("R-IME bridge disconnected")
            reply = json.loads(line)
            if not reply.get("ok"):
                raise RuntimeError(reply["error"])
            return reply

        session = call("create")["session"]
        try:
            state = call("set_schema", session=session, schema="rime_ime_pinyin")
            if not state["simplified"]:
                call("toggle_simplified", session=session)
            call("set_ascii", session=session, value=False)
            for character in "nihao":
                state = call("key", session=session, keycode=ord(character), modifiers=0)
            print(json.dumps(state["candidates"], ensure_ascii=False))
            print(call("select", session=session, index=0)["commit"])
        finally:
            try:
                call("destroy", session=session)
            finally:
                bridge.stdin.close()
        # Context manager waits for EOF and closes the remaining pipe.


if __name__ == "__main__":
    main()
