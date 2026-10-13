"""Reset the Ranked leaderboards (for a release that starts them again).

    python lb_reset.py --data ./data                 # dry run: what would go
    python lb_reset.py --data ./data --yes           # reset (backups first)
    python lb_reset.py --data ./data --restore lb_rows_before_20261020T120000Z --yes

The leaderboards are the lb_rows table of <data>/community.db (leaderboards.py).
By default only the ranking views go (leaderboards.json names ending in
_RANKING: the match type rankings, the total and the weekly one); the
Prestige views (32, 33) and the GameSpy one (8) stay. --all resets every view,
--views 1,2,17 just those.

Before anything is deleted: a full copy of the database next to it
(community.db.lb-reset-<UTC time>.bak, SQLite's online backup - safe while
the server runs) and the rows that go, kept in the database as
lb_rows_before_<UTC time>. --restore <that table> puts them back (the views
it holds: their current rows are replaced).

The server can keep running (the database is in WAL mode; the reset is one
transaction). Players' accounts, friends and Community Creations aren't touched.
"""
import argparse
import datetime
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def views_from_json():
    with open(os.path.join(HERE, "leaderboards.json"), encoding="utf-8") as f:
        return {int(k): v.get("name", "") for k, v in json.load(f).items()}


def counts(db, views):
    marks = ",".join("?" * len(views))
    rows = db.execute("SELECT view, COUNT(*) FROM lb_rows WHERE view IN (%s) GROUP BY view" % marks, views).fetchall()
    return dict(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data", required=True, help="the server's --data folder (holds community.db)")
    ap.add_argument("--all", action="store_true", help="every view, the Prestige ones too")
    ap.add_argument("--views", help="only these view ids (comma separated)")
    ap.add_argument("--restore", metavar="TABLE", help="put back the rows a reset kept in TABLE")
    ap.add_argument("--yes", action="store_true", help="do it (without: a dry run)")
    a = ap.parse_args()

    path = os.path.join(a.data, "community.db")
    if not os.path.isfile(path):
        sys.exit("no %s" % path)
    db = sqlite3.connect(path, timeout=30)
    names = views_from_json()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")

    if a.restore:
        if not a.restore.startswith("lb_rows_before_") or not a.restore[15:].replace("T", "").replace("Z", "").isdigit():
            sys.exit("not a reset's table: %s" % a.restore)
        if not db.execute("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", (a.restore,)).fetchone():
            sys.exit("no table %s" % a.restore)
        views = [r[0] for r in db.execute("SELECT DISTINCT view FROM %s" % a.restore)]
        n = db.execute("SELECT COUNT(*) FROM %s" % a.restore).fetchone()[0]
        print("%s: %d rows of %d views (%s)" % (a.restore, n, len(views), ", ".join(map(str, sorted(views)))))
        if not a.yes:
            print("dry run - add --yes to put them back")
            return
        marks = ",".join("?" * len(views))
        with db:
            db.execute("DELETE FROM lb_rows WHERE view IN (%s)" % marks, views)
            db.execute("INSERT INTO lb_rows SELECT * FROM %s" % a.restore)
        print("put back: %d rows" % n)
        return

    if a.views:
        views = sorted({int(v) for v in a.views.split(",") if v.strip()})
    elif a.all:
        views = sorted(names)
    else:
        views = sorted(v for v, name in names.items() if name.endswith("_RANKING"))
    have = counts(db, views)
    total = sum(have.values())
    for v in views:
        print("  view %2d %-40s %6d rows" % (v, names.get(v, "?"), have.get(v, 0)))
    kept = sorted(set(names) - set(views))
    print("reset: %d rows in %d views; kept: views %s" % (total, len(views), ", ".join(map(str, kept)) or "none"))
    if not a.yes:
        print("dry run - add --yes to reset (a backup of the database and of the rows comes first)")
        return

    # 1. the whole database, copied (SQLite's online backup)
    bak = path + ".lb-reset-" + stamp + ".bak"
    out = sqlite3.connect(bak)
    with out:
        db.backup(out)
    out.close()
    print("backup: %s" % bak)
    # 2. the rows that go, kept in the database - and gone, in one transaction
    table = "lb_rows_before_" + stamp
    marks = ",".join("?" * len(views))
    with db:
        db.execute("CREATE TABLE %s AS SELECT * FROM lb_rows WHERE view IN (%s)" % (table, marks), views)
        gone = db.execute("DELETE FROM lb_rows WHERE view IN (%s)" % marks, views).rowcount
    print("reset: %d rows gone (kept as table %s; undo: --restore %s --yes)" % (gone, table, table))


if __name__ == "__main__":
    main()
