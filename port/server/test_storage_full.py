"""Checks the "storage full" answer (12 Nov: a mod upload got a bare "the server said 500" while the storage
node's disk had 0 bytes free). A full disk now answers 507 with gamespy.STORAGE_FULL_TEXT in "error" (the
launcher, the phone app and the Mod Maker show it as it is), half-stored writes are rolled back, and the logs say
STORAGE FULL. Part one runs in this process; part two starts a private core + storage node on test ports with
throwaway data folders - the node told its disk is full by SVR2011_TEST_STORAGE_FREE.

    python test_storage_full.py [--ports 8556]   (that port: the core; the next one: the node)
"""
import argparse
import errno
import io
import json
import os
import secrets
import subprocess
import sys
import tempfile
import urllib.error
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gamespy as gs  # noqa: E402
from test_mod_admin_file import jpeg, multipart, register, request, svrmod, wait_up  # noqa: E402

PY = sys.executable
FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        FAILS.append(what)


class FullFile(io.BytesIO):
    def write(self, b):
        raise OSError(errno.ENOSPC, "No space left on device")


class FlakyBlobs:
    """Blobs in memory; `full` makes every put raise StorageFull."""

    def __init__(self):
        self.d, self.full = {}, False

    def exists(self, sha):
        return sha in self.d

    def put(self, sha, data, packed):
        if self.full:
            raise gs.StorageFull("test: full")
        self.d[sha] = data

    def get(self, sha):
        return (self.d[sha], False) if sha in self.d else None

    def delete(self, sha):
        self.d.pop(sha, None)


def part_one(tmp):
    check(gs.disk_full(OSError(errno.ENOSPC, "x")), "disk_full: ENOSPC")
    check(not gs.disk_full(OSError(errno.ENOENT, "x")), "disk_full: not for a missing file")
    check(gs.disk_full(urllib.error.HTTPError("u", 507, "full", {}, None)), "disk_full: the node's 507")
    check(not gs.disk_full(urllib.error.HTTPError("u", 500, "x", {}, None)), "disk_full: not for a 500")

    blobs = gs.LocalBlobs(str(tmp / "blobs"))
    gs.open = lambda path, mode="r", *a, **k: FullFile() if "w" in mode else open(path, mode, *a, **k)
    try:
        blobs.put("ab" * 32, b"data", False)
        check(False, "LocalBlobs.put on a full disk raises StorageFull")
    except gs.StorageFull:
        check(True, "LocalBlobs.put on a full disk raises StorageFull")
    finally:
        del gs.open
    left = list((tmp / "blobs").rglob("*.tmp"))
    check(not left, "no half-written .tmp left %s" % (left or ""))

    fb = FlakyBlobs()
    store = gs.Store(str(tmp / "store"), blobs=fb)
    with store.lock:
        a = store._put_blob(b"A" * 100, compress=False)
        store.db.commit()
    fb.full = True
    try:
        with store.lock:
            store._put_blob(b"A" * 100, compress=False)  # (A counted again: not committed)
            store._put_blob(b"B" * 100, compress=False)  # (the disk is full)
        check(False, "a store on a full disk raises")
    except gs.StorageFull:
        check(True, "a store on a full disk raises")
    with store.lock:
        store.db.commit()  # (an unrelated commit later must not keep the half-done write)
        refs = store.db.execute("SELECT refs FROM blobs WHERE sha = ?", (a,)).fetchone()[0]
    check(refs == 1, "the half-done write rolled back: A's refs stay 1 (%d)" % refs)


def start(tmp, name, cmd, env=None):
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            env=dict(os.environ, **(env or {})))


def upload(url, token):
    img = jpeg()
    body, ctype = multipart({"name": "Full test", "version": "1.0", "types": "superstar", "tags": "test"},
                            dict({"svrmod": ("full_test.svrmod", svrmod("1.0"), "application/zip")},
                                 **{"img%d" % n: ("img%d.jpg" % n, img, "image/jpeg") for n in range(4)}))
    st, out = request(url + "/api/mods", "POST", body, {"Authorization": "Bearer " + token, "Content-Type": ctype})
    try:
        return st, json.loads(out or b"{}")
    except ValueError:
        return st, {"raw": out[:200]}


def part_two(tmp, core_port, node_port):
    key = secrets.token_hex(32)
    (tmp / "key.txt").write_text(key)
    for d in ("core", "node"):
        (tmp / d).mkdir()
    node_cmd = [PY, str(HERE / "server.py"), "--role", "storage", "--host", "127.0.0.1", "--port", str(node_port),
                "--data", str(tmp / "node"), "--storage-key", str(tmp / "key.txt"), "--log", str(tmp / "node.log")]
    procs = {}
    try:
        procs["node"] = start(tmp, "node", node_cmd, {"SVR2011_TEST_STORAGE_FREE": "1000"})
        procs["core"] = start(tmp, "core", [
            PY, str(HERE / "server.py"), "--role", "core", "--test-open-accounts", "--host", "127.0.0.1",
            "--port", str(core_port), "--data", str(tmp / "core"), "--storage-url", "http://127.0.0.1:%d" % node_port,
            "--storage-key", str(tmp / "key.txt"), "--log", str(tmp / "core.log")])
        url, node = "http://127.0.0.1:%d" % core_port, "http://127.0.0.1:%d" % node_port
        check(wait_up(node, procs["node"], "/__health") and wait_up(url, procs["core"]),
              "private core %d + node %d up (the node's disk 'full')" % (core_port, node_port))
        acc = register(url, "sf" + secrets.token_hex(3))
        check(bool(acc.get("token")), "an account")

        st, out = upload(url, acc["token"])
        check(st == 507 and out.get("error") == gs.STORAGE_FULL_TEXT and out.get("code") == "storage_full",
              "the upload on a full node: 507 + the storage-full text (%d %s)" % (st, out.get("error") or out))
        mods = json.loads(request(url + "/api/mods")[1]).get("mods", [])
        check(not mods, "no mod listed after it (%d)" % len(mods))
        st, _ = request(node + "/blob/" + "cd" * 32, "PUT", b"x" * 10, {"X-Storage-Key": key})
        check(st == 507, "the node's own PUT: 507 (%d)" % st)
        core_log = (tmp / "core.log").read_text(encoding="utf-8", errors="replace")
        node_log = (tmp / "node.log").read_text(encoding="utf-8", errors="replace")
        check("STORAGE FULL" in core_log and "/api/mods" in core_log, "the core's log: STORAGE FULL on /api/mods")
        check(node_log.count("STORAGE FULL") == 1, "the node's log: STORAGE FULL once a minute (%d)" %
              node_log.count("STORAGE FULL"))
        check("Traceback" not in core_log, "no traceback in the core's log")

        procs["node"].kill()
        procs["node"].wait(10)
        procs["node"] = start(tmp, "node", node_cmd)  # (room made)
        check(wait_up(node, procs["node"], "/__health"), "the node again with room")
        st, out = upload(url, acc["token"])
        check(st == 200 and out.get("ok"), "the upload after room is made: 200 (%d %s)" % (st, out.get("error")))
        mods = json.loads(request(url + "/api/mods")[1]).get("mods", [])
        check(len(mods) == 1, "one mod listed (%d)" % len(mods))
        if mods:
            st, got = request(url + "/api/mods/%d/file" % mods[0]["id"])
            check(st == 200 and len(got) == mods[0].get("size", len(got)), "its download works (%d)" % st)
    finally:
        for p in procs.values():
            if p.poll() is None:
                p.kill()
                p.wait(10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", type=int, default=8556)
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="svr_storage_full_"))
    part_one(tmp)
    part_two(tmp, a.ports, a.ports + 1)
    print("--- %s (%s)" % ("all ok" if not FAILS else "%d FAILED" % len(FAILS), tmp))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
