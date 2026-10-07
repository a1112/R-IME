# Local input method control API implementation plan

**Goal:** expose R-IME to local scripts and C++ applications through a documented API and `r-ime-ctl`, then commit and push the completed Windows work and control interface.

**Architecture:** reuse the current-user Unix socket / Windows named pipe and version-1 engine protocol. A public C++ `ControlClient` owns one persistent connection; a JSON-lines CLI bridge lets other languages retain independent input sessions. Configuration changes use the worker's existing validation, staged deployment and rollback, with optimistic concurrency checks.

**Tech stack:** C++20, CMake, nlohmann/json, existing IPC client and librime worker. No network service or new UI dependency.

## Accepted scope

The user selected “本地 API + 命令行工具” after being offered status, scheme/mode, configuration and phrase controls. Defaults affect subsequent input sessions; the API cannot switch an unrelated application's active system input method or submit into its document. Input operations return text to the caller and only address sessions owned by that connection.

## Tasks

1. Add worker `configuration` snapshots (config + UTF-8 phrase TSV + diagnostics) on Linux and Windows. Add optional `expected_config` / `expected_phrases` checks before `configure` mutates data. Write integration assertions first, verify the missing operation fails, then implement. A stale write must fail without deployment, file writes or session loss.
2. Add public `include/rime_ime/control.hpp` and a CMake library. Wrap raw requests and provide configuration patch/set, phrase list/upsert/remove/replace, backup/restore. Validate inputs before connecting; use two-second ordinary timeouts and a 180-second deployment timeout. Never replay failed mutations. Verify with real private-worker integration tests and offline validation tests.
3. Add `r-ime-ctl` commands: `status`, `config get/set/import`, `schema`, `mode`, `phrases list/set/remove/import/export`, `backup`, `restore`, and persistent `rpc` over UTF-8 JSON lines. Success and errors are JSON, exit codes distinguish operational failure and usage. Bound input size, preserve Unicode arguments on Windows, and keep each bridge connection alive through EOF. Add command tests before implementation, including malformed JSON followed by a valid request and cross-client session rejection.
4. Install/package the executable, public headers and link libraries. Document C++ and Python usage, mode/default semantics, protocol framing, errors and deployment invalidation. Run the full Windows suite serially, build 32-bit TSF, inspect the final diff, commit and push the existing branch. Keep build/package operations separate from running tests.

## Acceptance

- A script can query status, change one validated setting without overwriting a concurrent edit, manage phrases and export/restore a backup.
- A persistent bridge can create a session, type `nihao`, read candidates, select one and receive `你好`; another bridge cannot operate on its session.
- Invalid settings/phrases/requests do not corrupt user data, and a disconnected request is never replayed.
- Both Windows package and CMake install include the CLI; all existing Windows tests continue passing.
- Existing Windows appearance/TSF changes are included in the requested commit and push. Linux runtime validation is reported accurately if no Linux host is available.
