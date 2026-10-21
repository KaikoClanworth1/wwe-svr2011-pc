#!/bin/bash
# One command for vps_cutover.md section B on the VPS (run as root, after the PC
# app is stopped, the database copied up as ~/community-cutover.db and the PC
# restarted as the storage node):
#   sudo bash ~/svr2011-src/port/server/vps_switch.sh /home/<user>/community-cutover.db
# Puts the database in place, starts the core, adds the nginx include (backup first;
# put back automatically if nginx -t fails) and runs the checks.
# Undo: sudo bash ~/svr2011-src/port/server/vps_switch.sh --rollback
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
inc='include /etc/nginx/svr2011/svr-location.conf;'
conf="$(grep -lR 'server_name[^;]*sho-ti\.me' /etc/nginx/conf.d /etc/nginx/sites-enabled 2>/dev/null | head -1 || true)"
[ -n "$conf" ] || { echo "no nginx file with server_name sho-ti.me found"; exit 1; }
bak=/root/$(basename "$conf").bak-cutover

if [ "${1:-}" = --rollback ]; then
    [ -f "$bak" ] && cp -a "$bak" "$conf" && nginx -t && systemctl reload nginx && echo "nginx: back to the PC"
    systemctl disable --now svr2011 || true
    echo "The core is stopped. On the PC: restore config.json.bak-cutover / service.py.bak-cutover and start the app."
    exit 0
fi

db="${1:?give the copied database, e.g. /home/<user>/community-cutover.db}"
[ -s "$db" ] || { echo "$db not found"; exit 1; }

echo "== the database"
install -m 640 -o svr2011 -g svr2011 "$db" /var/lib/svr2011/community.db
command -v restorecon >/dev/null && restorecon /var/lib/svr2011/community.db || true
command -v sqlite3 >/dev/null && sudo -u svr2011 sqlite3 /var/lib/svr2011/community.db "PRAGMA integrity_check;" || true

echo "== the PC's storage node over the tunnel"
curl -fsS --max-time 5 http://10.8.0.2:8102/__health || { echo; echo "the PC node doesn't answer - start it first (role storage)"; exit 1; }
echo

echo "== the core"
systemctl enable --now svr2011
sleep 3
curl -fsS http://127.0.0.1:8101/__health; echo
tail -n 15 /var/log/svr2011/server.log

echo "== nginx ($conf)"
if grep -qF "$inc" "$conf"; then
    echo "  the include is there already"
else
    cp -a "$conf" "$bak"
    # the include goes on the line before the first "location /" inside the sho-ti.me server block
    # (the HTTPS block: server_name sho-ti.me and listen 443 - not a port-80 redirect block)
    awk -v inc="    $inc" '
        /^[ \t]*server[ \t]*\{/ { named = 0; tls = 0 }
        /server_name[^;]*sho-ti\.me/ { named = 1 }
        /listen[^;]*443/ { tls = 1 }
        named && tls && !done && /^[ \t]*location[ \t]+\/[ \t]*\{/ { print inc; done = 1 }
        { print }
        END { if (!done) exit 3 }' "$bak" > "$conf" || { cp -a "$bak" "$conf"; echo "no 'location / {' found - add the include by hand"; exit 1; }
    if nginx -t; then systemctl reload nginx; echo "  include added (backup: $bak)"
    else cp -a "$bak" "$conf"; echo "  nginx -t failed - put back as it was"; exit 1; fi
fi

echo "== from the outside"
sleep 1
curl -fsS --max-time 10 https://sho-ti.me/svr/api/status; echo
echo
echo "Done. Next: sign in at https://sho-ti.me/svr-admin/, then the game checks (sign in, Community Creations, one download, one upload)."
