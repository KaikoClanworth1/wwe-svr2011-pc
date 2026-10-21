"""Tests the dashboard's own sign-in (admin_auth.py) against a running local server:
    python server.py --port 8491 --data <scratch>/data --local-admin      (after --set-admin-password)
    python test_admin_auth.py --url http://127.0.0.1:8491 --password <it>
Never against the live server: it locks addresses out on purpose. Requests come straight from
this PC, with X-Forwarded-For where nginx would set it (the client's address)."""
import argparse
import http.client
import json
import re
import sys
import urllib.parse

ADMIN_GETS = ["/api/admin/stats", "/api/admin/uploads", "/api/admin/users", "/api/admin/mods", "/api/admin/live",
              "/api/admin/log", "/api/admin/settings", "/api/admin/leaderboard"]
ADMIN_POSTS = ["/api/admin/settings", "/api/admin/restart", "/api/admin/mod/1/hide", "/api/admin/upload/1/hide",
               "/api/admin/user/1/ban"]
P = "/svr-admin"
fails = []


def req(url, method, path, body=None, headers=None):
    u = urllib.parse.urlparse(url)
    c = http.client.HTTPConnection(u.hostname, u.port, timeout=15)
    h = dict(headers or {})
    if isinstance(body, dict):
        body = urllib.parse.urlencode(body)
        h["Content-Type"] = "application/x-www-form-urlencoded"
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, data


def expect(what, ok):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        fails.append(what)


def login(url, password, ip, name="admin"):
    return req(url, "POST", P + "/login", {"name": name, "password": password}, {"X-Forwarded-For": ip})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--name", default="admin", help="(--admin-account servers) the account's name")
    a = ap.parse_args()
    url = a.url

    # no session: every dashboard route under /svr-admin/ is closed, whatever the headers say
    s, h, _ = req(url, "GET", P + "/", headers={"X-Forwarded-For": "203.0.113.9"})
    expect("dashboard page without a session -> 303 to the sign-in (%d %s)" % (s, h.get("location")),
           s == 303 and h.get("location", "").endswith(P + "/login"))
    for path in ADMIN_GETS:
        s, _, _ = req(url, "GET", P + path, headers={"X-Forwarded-For": "203.0.113.9"})
        expect("GET %s%s without a session -> 401 (%d)" % (P, path, s), s == 401)
    for path in ADMIN_POSTS:
        s, _, _ = req(url, "POST", P + path, body="{}", headers={"X-Forwarded-For": "203.0.113.9",
                                                                 "Content-Type": "application/json"})
        expect("POST %s%s without a session -> 401 (%d)" % (P, path, s), s == 401)
    for hdrs in ({"X-Forwarded-For": "127.0.0.1"}, {"X-Portal-Role": "admin", "X-Forwarded-For": "203.0.113.9"},
                 {"X-Portal-Role": "admin"}, {}):
        s, _, _ = req(url, "GET", P + "/api/admin/stats", headers=hdrs)
        expect("/svr-admin/ with only headers %s -> 401 (%d)" % (sorted(hdrs), s), s == 401)
    # the old routes: through a proxy (X-Forwarded-For) never admin by itself; direct = --local-admin
    s, _, _ = req(url, "GET", "/api/admin/stats", headers={"X-Forwarded-For": "203.0.113.9"})
    expect("old route with X-Forwarded-For alone -> 403 (%d)" % s, s == 403)
    s, _, _ = req(url, "GET", "/api/admin/stats")
    expect("old route direct (--local-admin, the SSH tunnel) -> 200 (%d)" % s, s == 200)

    # sign-in page
    s, h, body = req(url, "GET", P + "/login")
    expect("sign-in page -> 200 with a form, framing denied (%d)" % s,
           s == 200 and b'type="password"' in body and h.get("x-frame-options") == "DENY")

    # wrong passwords from one address: locked out after 5, even with the right one then
    bad_ip = "198.51.100.7"
    for i in range(5):
        s, h, _ = login(url, "wrong-%d" % i, bad_ip, a.name)
        expect("wrong password %d -> back to the form, no cookie (%s)" % (i + 1, h.get("location")),
               s == 303 and "e=wrong" in h.get("location", "") and "set-cookie" not in h)
    s, h, _ = login(url, a.password, bad_ip, a.name)
    expect("right password from the locked-out address -> refused (%s)" % h.get("location"),
           s == 303 and "e=locked" in h.get("location", "") and "set-cookie" not in h)

    # the right password from another address: a session
    s, h, _ = login(url, a.password, "192.0.2.10", a.name)
    cookie = h.get("set-cookie", "")
    expect("right password -> 303 to the dashboard with a cookie (%s)" % h.get("location"),
           s == 303 and h.get("location", "").endswith(P + "/") and "svr_admin=" in cookie)
    for flag in ("HttpOnly", "Secure", "SameSite=Strict", "Path=/svr-admin/", "Max-Age="):
        expect("cookie has %s" % flag, flag.lower() in cookie.lower())
    jar = {"Cookie": cookie.split(";")[0], "X-Forwarded-For": "192.0.2.10"}

    s, _, body = req(url, "GET", P + "/", headers=jar)
    m = re.search(rb'name="csrf-token" content="([^"]+)"', body)
    expect("dashboard page with the session -> 200 with its CSRF token (%d)" % s, s == 200 and m)
    csrf = m.group(1).decode() if m else ""
    for path in ADMIN_GETS:
        s, _, _ = req(url, "GET", P + path, headers=jar)
        expect("GET %s%s with the session -> 200 (%d)" % (P, path, s), s == 200)
    body = json.dumps({"invite_life": 600})
    s, _, _ = req(url, "POST", P + "/api/admin/settings", body, dict(jar, **{"Content-Type": "application/json"}))
    expect("settings POST with the session but no CSRF token -> 403 (%d)" % s, s == 403)
    s, _, _ = req(url, "POST", P + "/api/admin/settings", body,
                  dict(jar, **{"Content-Type": "application/json", "X-CSRF-Token": "forged"}))
    expect("settings POST with a wrong CSRF token -> 403 (%d)" % s, s == 403)
    s, _, _ = req(url, "POST", P + "/api/admin/settings", body,
                  dict(jar, **{"Content-Type": "application/json", "X-CSRF-Token": csrf}))
    expect("settings POST with the session and its CSRF token -> 200 (%d)" % s, s == 200)
    s, _, _ = req(url, "POST", P + "/api/admin/restart", "", dict(jar, **{"X-CSRF-Token": "forged"}))
    expect("restart POST with a wrong CSRF token -> 403 (%d, the server keeps running)" % s, s == 403)
    s, _, _ = req(url, "GET", "/api/admin/stats", headers={"Cookie": jar["Cookie"], "X-Forwarded-For": "192.0.2.10"})
    expect("the session's cookie on an old route through a proxy -> 403 (%d)" % s, s == 403)

    # sign out: the session is gone
    s, h, _ = req(url, "POST", P + "/logout", "", dict(jar, **{"X-CSRF-Token": csrf}))
    expect("sign out -> 303 to the sign-in, cookie cleared (%d)" % s, s == 303 and "max-age=0" in h.get("set-cookie", "").lower())
    s, _, _ = req(url, "GET", P + "/api/admin/stats", headers=jar)
    expect("the old cookie after signing out -> 401 (%d)" % s, s == 401)

    # every address: 20 failures from anywhere lock every sign-in
    for i in range(20):
        login(url, "spray-%d" % i, "203.0.113.%d" % (100 + i), a.name)
    s, h, _ = login(url, a.password, "192.0.2.77", a.name)
    expect("after 20 failures from anywhere, the right password from a new address -> locked (%s)" % h.get("location"),
           "e=locked" in h.get("location", ""))

    print("\n%d failed" % len(fails) if fails else "\nall passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
