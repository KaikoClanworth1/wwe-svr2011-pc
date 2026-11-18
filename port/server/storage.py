"""Where the server keeps its big files (the dashboard's Settings -> Storage).

Kinds (each its own folder; community.db, settings.json and the rest stay in --data):
  blobs    Community Creations files, Discovery mods, entrance songs and movies. One folder for all of them: a file
           is stored once by its content (sha-256), whichever part uses it, so they can't be split by kind.
  reports  problem reports and suggestions, their pictures, matches and the mods sent with them.
  backups  the account database's backups (signup.py, before each account change that needs one).

The choice is kept in <data>/storage.json: {"<kind>": {"path": "...", "old": ["..."]}}. No entry = <data>/<kind>.

A change MOVES the files, copy first: every file is copied to the new folder (progress shown), then, with that
kind's writes held (its lock), the files that changed meanwhile are copied again, every file is checked (same
size in the new folder), and only then does the server switch to the new folder. The old folder is never read
again and never deleted by this; it is listed under "old copies", and the dashboard deletes one only after a
typed confirm. A failed copy changes nothing: the server keeps the old folder.

The storage node (--role storage) has the same, for its blobs and reports, behind its key (server.py
storage_app: /__storage...), so the core's dashboard can move the node's folders too.
"""
import json
import os
import shutil
import threading
import time

KINDS = {
    "blobs": ("Community Creations, Discovery mods, entrance songs and movies",
              "one folder for all of them: a file is stored once by its content, whichever part uses it"),
    "reports": ("Problem reports and suggestions", "their pictures, match folders and the mods sent with them"),
    "backups": ("Account database backups", "copies of community.db made before account changes"),
}
RESERVE = 512 << 20   # (a move needs the files' size + this free on the new disk)
SIZE_CACHE = 60       # seconds a folder's measured size is kept
CONFIRM_WORD = "DELETE"


def folder_size(path):
    """(bytes, files) under path; (0, 0) when it is missing."""
    total = files = 0
    for root, _dirs, names in os.walk(path):
        for n in names:
            try:
                total += os.stat(os.path.join(root, n)).st_size
                files += 1
            except OSError:
                pass
    return total, files


def _same_or_inside(a, b):
    """a is b, or inside it."""
    a, b = os.path.normcase(os.path.abspath(a)), os.path.normcase(os.path.abspath(b))
    return a == b or a.startswith(b.rstrip("\\/") + os.sep)


class Storage:
    """The folders of one server (the core or the node). hooks[kind] = (lock, switch(new_path)): the lock holds
    that kind's writes; switch points the running server at the new folder (called with the lock held)."""

    def __init__(self, data, kinds, log=print):
        self.data = os.path.abspath(data)
        self.kinds = list(kinds)
        self.log = log
        self.path_file = os.path.join(self.data, "storage.json")
        self.hooks = {}
        self.job = None          # the move in progress / the last one: a dict (status())
        self.job_lock = threading.Lock()
        self.sizes = {}          # path -> (at, bytes, files)
        self.conf = self._load()

    # -- the stored choice

    def _load(self):
        try:
            with open(self.path_file, encoding="utf-8") as f:
                conf = json.load(f)
            return conf if isinstance(conf, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save(self):
        tmp = self.path_file + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.conf, f, indent=1, sort_keys=True)
        os.replace(tmp, self.path_file)

    def default(self, kind):
        return os.path.join(self.data, kind)

    def path(self, kind):
        p = (self.conf.get(kind) or {}).get("path")
        return p if p else self.default(kind)

    def old(self, kind):
        return list((self.conf.get(kind) or {}).get("old", []))

    def hook(self, kind, lock, switch):
        self.hooks[kind] = (lock, switch)

    # -- what the page shows

    def measure(self, path, fresh=False):
        hit = self.sizes.get(path)
        if hit and not fresh and time.time() - hit[0] < SIZE_CACHE:
            return hit[1], hit[2]
        size, files = folder_size(path)
        self.sizes[path] = (time.time(), size, files)
        return size, files

    @staticmethod
    def free(path):
        p = path
        while p and not os.path.isdir(p):  # (a folder not made yet: its disk)
            up = os.path.dirname(p)
            if up == p:
                break
            p = up
        try:
            du = shutil.disk_usage(p)
            return du.free, du.total
        except OSError:
            return None, None

    def info(self):
        out = []
        for kind in self.kinds:
            p = self.path(kind)
            size, files = self.measure(p)
            free, total = self.free(p)
            olds = []
            for o in self.old(kind):
                osize, ofiles = self.measure(o) if os.path.isdir(o) else (0, 0)
                olds.append({"path": o, "exists": os.path.isdir(o), "bytes": osize, "files": ofiles})
            out.append({"kind": kind, "label": KINDS[kind][0], "help": KINDS[kind][1], "path": p,
                        "default": self.default(kind), "is_default": p == self.default(kind),
                        "bytes": size, "files": files, "free": free, "total": total, "old": olds})
        return {"folders": out, "job": self.status()}

    def check(self, kind, new):
        """Can the files of kind go to new? -> {ok, problems, exists, writable, free, need, ...} (nothing changed)."""
        problems = []
        if kind not in self.kinds:
            return {"ok": False, "problems": ["Unknown kind."]}
        new = (new or "").strip()
        if not new or not os.path.isabs(new):
            return {"ok": False, "problems": ["Give a full path (e.g. E:\\SvR2011 Data\\blobs or /srv/svr2011/blobs)."]}
        new = os.path.abspath(new)
        cur = os.path.abspath(self.path(kind))
        exists = os.path.isdir(new)
        if os.path.normcase(new) == os.path.normcase(cur):
            problems.append("That is the folder in use now.")
        elif _same_or_inside(new, cur):
            problems.append("The new folder is inside the one in use now.")
        elif _same_or_inside(cur, new):
            problems.append("The folder in use now is inside the new one.")
        if os.path.normcase(new) == os.path.normcase(self.data):
            problems.append("Not the data folder itself: give a folder of its own.")
        for other in self.kinds:
            if other != kind and (_same_or_inside(new, self.path(other)) or _same_or_inside(self.path(other), new)):
                problems.append("It overlaps the %s folder (%s)." % (other, self.path(other)))
        back = any(os.path.normcase(new) == os.path.normcase(o) for o in self.old(kind))
        if exists and not back and any(True for _ in os.scandir(new)):
            problems.append("The folder isn't empty: pick an empty or a new folder (or one of this kind's old copies).")
        if not exists and os.path.exists(new):
            problems.append("That is a file, not a folder.")
        parent = new
        while parent and not os.path.exists(parent):
            up = os.path.dirname(parent)
            if up == parent:
                break
            parent = up
        writable = False
        if os.path.exists(parent) and not os.path.isdir(parent):
            if parent != new:
                problems.append("A part of that path is a file (%s)." % parent)
        elif not os.path.isdir(parent):
            problems.append("Its drive or parent folder doesn't exist.")
        else:
            probe = os.path.join(parent, ".svr2011_write_check_%d" % os.getpid())
            try:
                with open(probe, "wb") as f:
                    f.write(b"ok")
                os.remove(probe)
                writable = True
            except OSError as e:
                problems.append("The server can't write there (%s)." % (e.strerror or e))
        need, _files = self.measure(cur, fresh=True)
        free, total = self.free(new)
        if free is not None and free < need + RESERVE:
            problems.append("Not enough room: %s needed (the files + %d MB spare), %s free." % (
                human(need + RESERVE), RESERVE >> 20, human(free)))
        if self.busy():
            problems.append("A move is running: wait for it to end.")
        return {"ok": not problems, "problems": problems, "path": new, "exists": exists, "writable": writable,
                "free": free, "total": total, "need": need}

    # -- the move

    def busy(self):
        return bool(self.job and self.job["state"] in ("copying", "checking"))

    def status(self):
        return dict(self.job) if self.job else None

    def move(self, kind, new):
        """Starts the move in a thread -> (ok, error)."""
        with self.job_lock:
            c = self.check(kind, new)
            if not c["ok"]:
                return False, " ".join(c["problems"])
            if kind not in self.hooks:
                return False, "This server can't switch that folder while it runs."
            src = os.path.abspath(self.path(kind))
            self.job = {"kind": kind, "from": src, "to": c["path"], "state": "copying", "done_bytes": 0,
                        "total_bytes": c["need"], "done_files": 0, "total_files": self.measure(src)[1],
                        "started": time.time(), "ended": None, "error": ""}
            threading.Thread(target=self._run, args=(self.job,), name="storage-move", daemon=True).start()
            return True, ""

    def _copy_tree(self, job, src, dst, count):
        """Copies what is missing or different (size, mtime) in dst. -> files copied."""
        copied = 0
        for root, dirs, names in os.walk(src):
            rel = os.path.relpath(root, src)
            if rel == ".":
                dirs[:] = [d for d in dirs if d != "tmp"]  # (files on their way: not kept)
            out = dst if rel == "." else os.path.join(dst, rel)
            os.makedirs(out, exist_ok=True)
            for n in names:
                if n.endswith(".tmp"):
                    continue  # (a file being written: the final pass, with writes held, sees it done)
                a, b = os.path.join(root, n), os.path.join(out, n)
                try:
                    sa = os.stat(a)
                except OSError:
                    continue  # (deleted meanwhile)
                try:
                    sb = os.stat(b)
                    same = sb.st_size == sa.st_size and int(sb.st_mtime) == int(sa.st_mtime)
                except OSError:
                    same = False
                if not same:
                    tmp = b + ".svrmove"
                    shutil.copyfile(a, tmp)
                    shutil.copystat(a, tmp)
                    os.replace(tmp, b)
                    copied += 1
                if count:
                    job["done_bytes"] += sa.st_size
                    job["done_files"] += 1
        return copied

    @staticmethod
    def _verify(src, dst):
        """-> the first file missing or different in dst, or ''."""
        for root, dirs, names in os.walk(src):
            rel = os.path.relpath(root, src)
            if rel == ".":
                dirs[:] = [d for d in dirs if d != "tmp"]
            for n in names:
                if n.endswith(".tmp"):
                    continue
                a = os.path.join(root, n)
                b = os.path.join(dst, n) if rel == "." else os.path.join(dst, rel, n)
                try:
                    if os.stat(a).st_size != os.stat(b).st_size:
                        return os.path.join(rel, n)
                except FileNotFoundError:
                    if os.path.exists(a):
                        return os.path.join(rel, n)
        return ""

    def _run(self, job):
        kind, src, dst = job["kind"], job["from"], job["to"]
        lock, switch = self.hooks[kind]
        self.log("storage: moving %s from %s to %s (%s)" % (kind, src, dst, human(job["total_bytes"])))
        try:
            os.makedirs(dst, exist_ok=True)
            if os.path.isdir(src):
                self._copy_tree(job, src, dst, True)
            job["state"] = "checking"
            with lock:  # (writes held: what changed meanwhile, the check, the switch)
                if os.path.isdir(src):
                    again = self._copy_tree(job, src, dst, False)
                    bad = self._verify(src, dst)
                    if bad:
                        raise OSError("the copy of %s differs - nothing switched" % bad)
                else:
                    again = 0
                entry = self.conf.setdefault(kind, {})
                olds = [o for o in entry.get("old", []) if os.path.normcase(o) != os.path.normcase(dst)]
                if os.path.isdir(src) and src not in olds:
                    olds.append(src)
                entry["path"], entry["old"] = dst, olds
                self._save()
                switch(dst)
            self.sizes.pop(src, None)
            self.sizes.pop(dst, None)
            job["state"] = "done"
            self.log("storage: %s now in %s (%d file(s) copied again at the switch); the old copy stays in %s "
                     "until it is deleted on the dashboard" % (kind, dst, again, src))
        except Exception as e:  # (the old folder stays in use: nothing is lost)
            job["state"] = "failed"
            job["error"] = str(e)
            self.log("storage: moving %s to %s FAILED, still using %s: %s" % (kind, dst, src, e))
        job["ended"] = time.time()

    def delete_old(self, kind, path, confirm):
        """Deletes an old copy (listed under old, not in use, the typed word given) -> (ok, error, bytes freed)."""
        if confirm != CONFIRM_WORD:
            return False, "Type %s to confirm." % CONFIRM_WORD, 0
        if kind not in self.kinds or path not in self.old(kind):
            return False, "That isn't an old copy of this kind.", 0
        if self.busy():
            return False, "A move is running: wait for it to end.", 0
        for other in self.kinds:
            if _same_or_inside(self.path(other), path) or _same_or_inside(path, self.path(other)):
                return False, "That folder is in use (%s)." % other, 0
        if os.path.normcase(os.path.abspath(path)) == os.path.normcase(self.data) or _same_or_inside(self.data, path):
            return False, "That holds the data folder.", 0
        size, _ = self.measure(path, fresh=True) if os.path.isdir(path) else (0, 0)
        shutil.rmtree(path, ignore_errors=True)
        if os.path.isdir(path):
            return False, "Some files couldn't be deleted (in use?); the copy is still listed.", 0
        self.conf[kind]["old"] = [o for o in self.old(kind) if o != path]
        self._save()
        self.sizes.pop(path, None)
        self.log("storage: deleted the old %s copy %s (%s)" % (kind, path, human(size)))
        return True, "", size


def human(n):
    if n is None:
        return "?"
    for unit in ("bytes", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return ("%d %s" % (n, unit)) if unit == "bytes" else ("%.1f %s" % (n, unit))
        n /= 1024.0
