# Problem reports API

Report a problem in the launcher, the Mod Maker (and later the game / Android) sends its report to the online
server. The server is `port/server/reports.py` + `server.py` (`api_report`); the clients are owned by the Log File
Reporting work.

## POST {online_server}/api/reports

`online_server` is the players' setting (live: `https://sho-ti.me/svr`, so `/svr/api/reports`). No sign-in is
needed; an `Authorization: Bearer <token>` (optional, not sent by today's clients) only adds the account's name.

`multipart/form-data` parts:

| part | type | |
|---|---|---|
| `report` (or `report.json`) | `application/json`, UTF-8 | required, 64 KB at most |
| `zip` | `application/zip` | optional, 25 MB at most; the client's report zip (already redacted) |
| `image0` .. `image4` | PNG / JPEG / WebP (checked by their bytes) | optional, 8 MB each |

`report` fields: `format` (1), `app` (`launcher` / `modmaker` / `game` / `android`, required), `version` (required),
`platform`, `os`, `problem` (required), `happens`, `what`, `before`, `contact`, `signature` (the client's hint).
`what` / `before` are cut at 4000 characters, the rest at 200. Other fields are dropped.

Answers (JSON):
- `200 {"ok": true, "id": "20261024-153012-ab12cd", "signature": "<16 hex>", "seen_before": N}`:
  N = earlier reports with the same signature (0: new). A client can say "already reported N times, thanks".
- `400` bad input, `413` too large, `429` too many (5 in 10 minutes and 20 a day per address, and per account),
  `503` the storage node is down: `{"ok": false, "error": "<text to show>"}`. The client keeps its zip.

## The signature (duplicates)

`sha1(app | version | problem | line)[:16]`, where `line` is the first crash / error line in the zip (its
`crash*` files first, then `*.log` / `*.txt` lines with error / exception / fatal / crash / assert / unhandled /
traceback), made general: leading times, other times, `0x...` and 8-16 digit hex, thread / tid / pid numbers,
drive paths and the remaining numbers are taken out. No such line: the client's `signature` hint stands in.

## Where they are kept

On the machine that holds the Community Creations bytes (the storage node: the user's PC; `--role all`: here),
in its `--data` folder:

    reports/<yyyy-mm-dd>/<id>/report.json   the fields + id, received, signature, signature_line, seen_before,
                                            zip_size, images (never the sender's address)
    reports/<yyyy-mm-dd>/<id>/report.zip
    reports/<yyyy-mm-dd>/<id>/image0.png ...
    reports/index.jsonl                     one line per report (the list, and the duplicates' counts)
    reports/status.json                     the marks: id -> done / duplicate, or
                                            {status: fixed, note, commit, version, date}

The core passes a report to the node as one stored zip (`POST /reports` with `X-Storage-Key`); the node also
answers `GET /reports`, `GET /reports/<id>/<file>`, `POST /reports/<id>/mark` (the id may be a signature: every
report with it; `{status, fix}`) and `POST /reports/<id>/delete`. The sender's address stays in the core's memory,
for the rate limit only.

## Marking a fix

On the PC that keeps the reports (the storage node, or `--role all`), with the server running or not:

    python port/server/reports.py --data <the server's --data> mark <report id | signature> --fixed "how it was fixed" --commit abc1234 --version 2.1.6 [--date 2026-10-25]
    python port/server/reports.py --data <...> mark <id | signature> --done | --duplicate | --reopen
    python port/server/reports.py --data <...> delete <report id>
    python port/server/reports.py --data <...> list [--fixed]

The same from the dashboard's API (admin): `POST /api/admin/report/<id | signature>/mark` with
`{"status": "fixed", "fix": {"note", "commit", "version", "date"}}` (date: today when left out) and
`POST /api/admin/report/<id>/delete`.

## Dashboard

The Reports tab:
- "Fixed while you were away" first: the fixed reports, newest fix first, each with its problem, what happened, how it
  was fixed, the version it ships in, the commit, the date, its pictures and Details. Delete (and "Delete all shown")
  asks in place (a second click within 5 s) and removes the report's folder, index line and mark.
- Then the open reports grouped by signature (newest first): their count, the error line, picture thumbnails,
  Details (report.json), the zip, and Done / Duplicate / Reopen marks. Fixed ones leave this list.

## Deploying

- The VPS (core): `vps_install.sh` copies `reports.py` and writes the nginx location `/svr/api/reports`
  (`client_max_body_size 70m`).
- The PC storage node runs `server.py --role storage` from its own folder: `reports.py` must be beside it, and
  its server restarted, before the core's new code goes live (until then the core answers 503).

## Tests

`port/server/test_reports.py --url http://127.0.0.1:<core> --node-data <node --data>` against a private core
(`--role core --local-admin`) + node (`--role storage`) on scratch ports; never the live server.
