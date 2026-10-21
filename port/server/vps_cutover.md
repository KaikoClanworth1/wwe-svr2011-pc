# Moving the online server's core to the VPS - checklist

The plan behind it: `docs/ONLINE_VPS_PLAN.md`. After the switch:

```
players --https--> VPS nginx (sho-ti.me) --/svr/--> core (svr2011.service, 127.0.0.1:8101)
players --UDP 36500--> relay edge (svr-relay-edge, unchanged) <--10.8.0.1:36501--> core
core --(Community Creations file bytes only, WireGuard)--> PC storage node (10.8.0.2:8102)
```

Nothing here touches WireGuard, firewalld on the VPS (443 and UDP 36500 are open
already; 8101 / 8102 / 36501 stay private) or the relay edge's unit.

Files: `vps_install.sh` / `vps_uninstall.sh` (VPS), `pc_storage_setup.ps1` (PC).

## A. Preparation (no downtime, any day)

- [ ] **PC** - the server files of the version to run, as a tarball:
  ```
  cd "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011"
  git archive --format=tar.gz -o svr2011-server.tar.gz HEAD port/server
  scp svr2011-server.tar.gz <user>@152.53.32.77:
  ```
- [ ] **VPS** - unpack, look first, then install (unit disabled, nginx location not included):
  ```
  mkdir -p ~/svr2011-src && tar xzf ~/svr2011-server.tar.gz -C ~/svr2011-src
  cd ~/svr2011-src/port/server
  sudo bash vps_install.sh --dry-run
  sudo bash vps_install.sh
  systemctl is-enabled svr2011        # -> disabled
  sudo cat /etc/svr2011/storage_key   # copy the value for the PC (it is a secret)
  ```
- [ ] **VPS** - the dashboard's password (for https://sho-ti.me/svr-admin/ - at least 10 characters, a new
  one, not the online account's; asked twice, stored only as a hash in `/var/lib/svr2011/admin_password`):
  ```
  sudo svr2011-admin-password
  sudo ls -l /var/lib/svr2011/admin_password   # -> -rw------- svr2011
  ```
  Run it again any time to change it (no restart). Instead of a password of its own, the sign-in can be
  the admin's online account: add `--admin-account <name>` to the unit's `ExecStart`
  (`sudo systemctl edit --full svr2011`).
- [ ] **PC** - the same key, and see the switch's commands:
  ```
  cd "D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\server"
  powershell -ExecutionPolicy Bypass -File pc_storage_setup.ps1 -WhatIf
  powershell -ExecutionPolicy Bypass -File pc_storage_setup.ps1
  ```
- [ ] **PC** - the firewall rule for the storage node (admin PowerShell; harmless before the switch,
  nothing listens on 8102 yet):
  ```
  New-NetFirewallRule -DisplayName "SvR2011 storage node (WireGuard)" -Direction Inbound -Protocol TCP -LocalPort 8102 -RemoteAddress 10.8.0.1 -Action Allow -Profile Any
  ```
- [ ] **PC** - the Project Index change, prepared but not saved yet (`pc_storage_setup.ps1` prints it):
  `config.json` `bind_host` 10.8.0.2, `bind_port` 8102, `"role": "storage"`, `"storage_key": "<key file>"`,
  without `relay_udp_port` / `relay_edge`; `service.py` passes `--role` and `--storage-key`.
  Keep a copy of both files as they are now (`config.json.bak-cutover`, `service.py.bak-cutover`).

## B. Switch (a quiet hour; matches in progress keep playing P2P)

- [ ] 1. Announce it (Discord / the launcher news).
- [ ] 2. **PC** - stop the SvR 2011 Online app in Project Index. Check nothing listens any more:
  ```
  Get-NetTCPConnection -LocalPort 8101 -State Listen -ErrorAction SilentlyContinue   # (nothing)
  ```
- [ ] 3. **PC** - a consistent copy of the database (after the clean stop), then to the VPS:
  ```
  cd "F:\ClaudeProjects\Project Index\Apps\SvR2011\data"
  & "F:\ClaudeProjects\Project Index\venv\Scripts\python.exe" -c "import sqlite3; s=sqlite3.connect('community.db'); d=sqlite3.connect('community-cutover.db'); s.backup(d); d.close(); s.close()"
  scp community-cutover.db <user>@152.53.32.77:
  ```
  (`settings.json`, when there is one in that folder, goes along the same way.)
- [ ] 4. **VPS** - put it in place:
  ```
  sudo install -m 640 -o svr2011 -g svr2011 ~/community-cutover.db /var/lib/svr2011/community.db
  sudo restorecon /var/lib/svr2011/community.db
  sudo -u svr2011 sqlite3 /var/lib/svr2011/community.db "PRAGMA integrity_check;"   # -> ok (if sqlite3 is installed)
  ```
- [ ] 5. **PC** - save the prepared Project Index change (A) and start the app again: it is the storage
  node now. Check from the PC:
  ```
  Invoke-RestMethod http://10.8.0.2:8102/__health   # -> ok True, role storage
  ```
- [ ] 6. **VPS** - the core, and that it sees the node and the edge:
  ```
  curl -s http://10.8.0.2:8102/__health               # the node over the tunnel
  sudo systemctl enable --now svr2011
  sleep 3; curl -s http://127.0.0.1:8101/__health
  sudo tail -n 30 /var/log/svr2011/server.log           # "relay: edge linked", no errors
  ```
- [ ] 7. **VPS** - switch nginx: add the include in the sho-ti.me `server { }` block, **before** the
  portal's catch-all `location /`:
  ```
  sudo cp -a /etc/nginx/conf.d/project-index.conf /root/project-index.conf.bak-cutover   # (its real file name: nginx -T | grep "# configuration file")
  sudoedit /etc/nginx/conf.d/project-index.conf
  #     include /etc/nginx/svr2011/svr-location.conf;
  sudo nginx -t && sudo systemctl reload nginx
  ```
- [ ] 8. Smoke tests, from outside the VPS:
  ```
  curl -s https://sho-ti.me/svr/api/status             # the core answers (not the portal)
  ```
  - the launcher: sign in; Online tab shows the account
  - the game: ONLINE menu ("signed in as"), Community Creations list, one download (comes from the PC node)
  - one upload (a logo), then check it on the PC: a new file under `Apps\SvR2011\data\blobs`
  - a relayed test match (two games, UDP 36500)
  - the dashboard: https://sho-ti.me/svr-admin/ -> the sign-in page; the password -> the dashboard; Sign out.
    Without signing in every dashboard call answers 401:
    ```
    curl -s -o /dev/null -w "%{http_code}\n" https://sho-ti.me/svr-admin/api/admin/stats     # -> 401
    curl -s -o /dev/null -w "%{http_code}\n" -X POST https://sho-ti.me/svr-admin/api/admin/restart   # -> 401
    ```
    (Five wrong passwords lock that address out for 15 minutes, twenty from anywhere lock every sign-in;
    `admin:` lines in server.log. Don't test the lockout from your own address right before you need in.)
  - the dashboard over a tunnel still works too: `ssh -L 8101:127.0.0.1:8101 <user>@152.53.32.77`, then
    http://127.0.0.1:8101/
- [ ] 9. Watch `/var/log/svr2011/server.log` and the PC node's `server.log` for the first hour.

## C. Rollback (the first week)

- [ ] 1. **VPS** - nginx back: take the include line out (or restore `/root/project-index.conf.bak-cutover`),
  `sudo nginx -t && sudo systemctl reload nginx` - the portal path is used again.
- [ ] 2. **VPS** - stop the core and copy its database out:
  ```
  sudo systemctl disable --now svr2011
  sudo sqlite3 /var/lib/svr2011/community.db ".backup /tmp/community-back.db"   # (or cp community.db* while stopped)
  sudo chmod 644 /tmp/community-back.db
  ```
- [ ] 3. **PC** - stop the app, restore `config.json.bak-cutover` / `service.py.bak-cutover`, put the
  VPS database in place (files uploaded meanwhile are on the PC already: the node stored them):
  ```
  cd "F:\ClaudeProjects\Project Index\Apps\SvR2011\data"
  Rename-Item community.db community.db.before-rollback
  Remove-Item community.db-wal, community.db-shm -ErrorAction SilentlyContinue
  scp <user>@152.53.32.77:/tmp/community-back.db community.db
  ```
  then start the app (`--role all`, as before) and check https://sho-ti.me/svr/api/status.
- [ ] 4. Later, to remove the VPS side: `sudo bash vps_uninstall.sh --dry-run`, then without
  `--dry-run` (keeps the data and keys; `--purge` removes them).
