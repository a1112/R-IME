"""Read-only native contract test. Never register, enable, or load R-IME's COM DLL."""
import json
import subprocess
import sys
import winreg

GUID = "{1A5D0370-40A3-4865-90A6-324E1BB1855B}"


def registration_snapshot():
    result = {}

    def subtree(hive, view, name):
        try:
            with winreg.OpenKey(hive, name, 0, winreg.KEY_READ | view) as key:
                children, count, _ = winreg.QueryInfoKey(key)
                values = sorted(winreg.EnumValue(key, index) for index in range(count))
                nested = {winreg.EnumKey(key, index): subtree(hive, view, name + "\\" + winreg.EnumKey(key, index))
                          for index in range(children)}
                return {"values": values, "children": nested}
        except FileNotFoundError:
            return None

    for hive_name, hive in (("machine", winreg.HKEY_LOCAL_MACHINE), ("user", winreg.HKEY_CURRENT_USER)):
        for view_name, view in (("64", winreg.KEY_WOW64_64KEY), ("32", winreg.KEY_WOW64_32KEY)):
            for kind, name in (("COM", f"Software\\Classes\\CLSID\\{GUID}"),
                               ("TIP", f"Software\\Microsoft\\CTF\\TIP\\{GUID}")):
                result[f"{hive_name}/{view_name}/{kind}"] = subtree(hive, view, name)
    return result


def main():
    before = registration_snapshot()
    adapter = len(sys.argv) > 2 and sys.argv[2] == "--adapter-probe"
    process = subprocess.run([sys.argv[1]] + ([] if adapter else ["--query-profile"]),
                             capture_output=True, text=True, timeout=15, check=True)
    record = json.loads(process.stdout)
    if adapter:
        assert set(record) == {"registration", "preflight"}
        assert set(record["registration"]) == {"x64", "x86", "legacyInstallation"}
    else:
        assert set(record) == {"exists", "keyboardCategory", "enabled"}
        assert all(isinstance(value, bool) for value in record.values())
        if not record["exists"]:
            assert not record["keyboardCategory"] and not record["enabled"]
        else:
            # An explicit identical current-user value in both registry views
            # must agree with the helper's current-user enabled-state query.
            enabled = []
            for view in ("64", "32"):
                node = before[f"user/{view}/TIP"]
                for name in ("LanguageProfile", "0x00000804", "{80B722F7-FA50-4D8E-A084-E12A5B906585}"):
                    node = node["children"].get(name) if node else None
                if node:
                    enabled.extend(value for name, value, kind in node["values"]
                                   if name == "Enable" and kind == winreg.REG_DWORD and value in (0, 1))
            if len(enabled) == 2 and enabled[0] == enabled[1]:
                assert record["enabled"] == bool(enabled[0]), "TSF query missed the current-user enabled override"
    assert registration_snapshot() == before, "Read-only query altered COM or TIP registration"
    print(json.dumps({"query": record, "registryPreserved": True}))


if __name__ == "__main__":
    main()
