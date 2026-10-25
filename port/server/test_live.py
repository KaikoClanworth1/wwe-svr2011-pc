"""Checks the hosts' live session reports (POST /api/live) as the dashboard shows them: the match kind (Ranked /
Player, 2.1.6+; an older game's report has none) and the arena's name. Against a private server with
--local-admin only (it makes throwaway accounts):

    python test_live.py --server http://127.0.0.1:8497
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

FAILS = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def call(server, method, path, body=None, token=None):
    headers = {"Content-Type": "application/json"} if body is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(server + path, method=method, headers=headers,
                                 data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, timeout=30) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    server = ap.parse_args().server.rstrip("/")
    suffix, password = os.urandom(3).hex(), "pw-" + os.urandom(6).hex()
    hosts = {}
    for who in ("ranked", "player", "older"):
        st, acc = call(server, "POST", "/api/register", {"name": "live_%s_%s" % (who[:3], suffix), "password": password})
        check("register a %s host: 200" % who, st == 200 and acc.get("token"))
        hosts[who] = acc
    sessions = {"ranked": "A1" + suffix, "player": "B2" + suffix, "older": "C3" + suffix}
    for who, sid in sessions.items():
        body = {"session": sid, "state": "match", "rule": "rule 4D", "arena": "arena 11", "slots": 2,
                "version": "2.1.6", "network": "auto", "players": [{"name": "live_" + who[:3], "path": "host"}]}
        if who != "older":
            body["match_kind"] = who
            body["arena_mod"] = "KING OF THE RING 1998" if who == "player" else ""
        st, _ = call(server, "POST", "/api/live", body, hosts[who]["token"])
        check("live report from the %s host: 200" % who, st == 200)
    st, _ = call(server, "POST", "/api/live", {"session": "D4" + suffix, "match_kind": "<b>x</b>"}, hosts["older"]["token"])
    st, d = call(server, "GET", "/api/admin/live")
    rows = {v["session"]: v for v in d.get("sessions", [])} if st == 200 else {}
    check("admin live: the sessions listed", all(s in rows for s in sessions.values()))
    check("match kind: ranked / player / '' (older game)",
          rows.get(sessions["ranked"], {}).get("match_kind") == "ranked" and
          rows.get(sessions["player"], {}).get("match_kind") == "player" and
          rows.get(sessions["older"], {}).get("match_kind") == "")
    check("an unknown match kind is dropped", rows.get("D4" + suffix, {}).get("match_kind") == "")
    check("arena names: the game's (11 WRESTLEMANIA XXVI), a custom arena's own",
          rows.get(sessions["ranked"], {}).get("arena_name") == "WRESTLEMANIA XXVI" and
          rows.get(sessions["player"], {}).get("arena_name") == "KING OF THE RING 1998" and
          rows.get(sessions["player"], {}).get("arena_mod") is True)
    for who, sid in sessions.items():
        call(server, "POST", "/api/live", {"session": sid, "ended": True}, hosts[who]["token"])
    call(server, "POST", "/api/live", {"session": "D4" + suffix, "ended": True}, hosts["older"]["token"])
    st, d = call(server, "GET", "/api/admin/live")
    check("ended sessions leave the list", not any(v["session"] in sessions.values() for v in d.get("sessions", [])))
    print("\n%d failed" % len(FAILS) if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
