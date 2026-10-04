"""The game's Xbox LIVE leaderboards (stats views), kept here.

The game writes its players' stats (XSessionWriteStats: per view, property
values) and reads them back (XUserReadStats: given players; stats
enumerators: a page by rank or around a player). How each view's columns
take a write comes from the game's own configuration (its XLAST, in
leaderboards.json): "Sum" adds, "Last" replaces; the rating column (attribute
65534) orders the view, the rank (65535) and the name (65533) are worked out
here. Weekly views start again each week (ISO week).
"""
import datetime
import json
import os
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RANK, RATING, NAME = 65535, 65534, 65533


def week():
    y, w, _ = datetime.datetime.now(datetime.timezone.utc).isocalendar()
    return "%d-W%02d" % (y, w)


class Leaderboards:
    def __init__(self, store):
        self.db = store.db
        self.lock = store.lock
        with open(os.path.join(HERE, "leaderboards.json"), encoding="utf-8") as f:
            self.views = {int(k): v for k, v in json.load(f).items()}
        for v in self.views.values():  # (by property id)
            v["columns"] = {int(pid, 16): col for pid, col in v["columns"].items()}
        with self.lock:
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS lb_rows (
                    view INTEGER, xuid INTEGER, account INTEGER, rating INTEGER, columns TEXT, week TEXT,
                    updated REAL, PRIMARY KEY (view, xuid));
                CREATE INDEX IF NOT EXISTS lb_rank ON lb_rows (view, rating DESC, updated);
            """)
            self.db.commit()

    # -- writes ---------------------------------------------------------------

    def write(self, xuid, account, views):
        """views: [{"view": id, "props": [[property id, type, value], ...]}]."""
        done = 0
        with self.lock:
            for v in views:
                view = self.views.get(int(v.get("view", 0)))
                if not view:
                    continue
                vid = int(v["view"])
                row = self.db.execute("SELECT rating, columns, week FROM lb_rows WHERE view = ? AND xuid = ?",
                                      (vid, xuid)).fetchone()
                rating, cols, wk = (row[0], json.loads(row[1]), row[2]) if row else (0, {}, week())
                if view["reset"] == "Weekly" and wk != week():
                    rating, cols, wk = 0, {}, week()
                for prop in v.get("props", []):
                    pid, ptype, value = int(prop[0]), int(prop[1]), prop[2]
                    col = view["columns"].get(pid)
                    if not col:
                        continue
                    attr, agg = col[0], col[1]
                    if attr in (RANK, NAME):
                        continue
                    if attr == RATING:
                        rating = rating + int(value) if agg == "Sum" else int(value)
                        continue
                    key = str(attr)
                    old = cols.get(key, [ptype, 0])[1]
                    if agg == "Sum" and isinstance(value, (int, float)) and isinstance(old, (int, float)):
                        value = old + value
                    elif agg == "Max" and isinstance(value, (int, float)):
                        value = max(old, value) if isinstance(old, (int, float)) else value
                    elif agg == "Min" and isinstance(value, (int, float)):
                        value = min(old, value) if isinstance(old, (int, float)) and key in cols else value
                    cols[key] = [ptype, value]
                self.db.execute("INSERT OR REPLACE INTO lb_rows VALUES (?, ?, ?, ?, ?, ?, ?)",
                                (vid, xuid, account, rating, json.dumps(cols), wk, time.time()))
                done += 1
            self.db.commit()
        return done

    # -- reads ----------------------------------------------------------------

    def _alive(self, vid):
        """The rows that count (a weekly view: this week's)."""
        view = self.views.get(vid) or {}
        return ("AND week = ?", (week(),)) if view.get("reset") == "Weekly" else ("", ())

    def total(self, vid):
        cond, args = self._alive(vid)
        with self.lock:
            return self.db.execute("SELECT COUNT(*) FROM lb_rows WHERE view = ? " + cond, (vid,) + args).fetchone()[0]

    def _ranked(self, vid, where="", args=(), limit=None, offset=0):
        cond, cargs = self._alive(vid)
        sql = ("SELECT xuid, account, rating, columns, rank FROM (SELECT xuid, account, rating, columns, "
               "ROW_NUMBER() OVER (ORDER BY rating DESC, updated) AS rank FROM lb_rows WHERE view = ? " + cond +
               ") " + where + " ORDER BY rank")
        params = (vid,) + cargs + tuple(args)
        if limit is not None:
            sql += " LIMIT ? OFFSET ?"
            params += (int(limit), int(offset))
        with self.lock:
            return self.db.execute(sql, params).fetchall()

    def rows_for(self, vid, xuids):
        if not xuids:
            return []
        marks = ",".join("?" * len(xuids))
        return self._ranked(vid, "WHERE xuid IN (%s)" % marks, [int(x) for x in xuids])

    def page_by_rank(self, vid, start, count):
        return self._ranked(vid, limit=count, offset=max(0, int(start) - 1))

    def page_around(self, vid, xuid, count):
        mine = self._ranked(vid, "WHERE xuid = ?", [int(xuid)])
        rank = mine[0][4] if mine else 1
        return self.page_by_rank(vid, max(1, rank - int(count) // 2), count)

    def page_by_rating(self, vid, rating, count):
        cond, cargs = self._alive(vid)
        with self.lock:
            above = self.db.execute("SELECT COUNT(*) FROM lb_rows WHERE view = ? AND rating > ? " + cond,
                                    (vid, int(rating)) + cargs).fetchone()[0]
        return self.page_by_rank(vid, above + 1, count)
