#!/bin/bash
# Undoes vps_install.sh: stops and removes svr2011.service, the nginx location
# file and /opt/svr2011. The data (/var/lib/svr2011), the logs and the keys
# (/etc/svr2011) and the svr2011 user stay unless --purge.
#   sudo bash vps_uninstall.sh --dry-run
#   sudo bash vps_uninstall.sh [--purge]
# It refuses while nginx still includes the location (take the include line out
# and reload nginx first - vps_cutover.md, Rollback). The SELinux boolean
# httpd_can_network_connect stays on (nginx's proxy to the portal uses it too).
# Never touches WireGuard, firewalld or the relay edge.
set -uo pipefail

dry=0; purge=0
for a in "$@"; do
    case "$a" in
        --dry-run) dry=1 ;;
        --purge) purge=1 ;;
        *) echo "unknown option $a (--dry-run, --purge)"; exit 1 ;;
    esac
done
run() { if [ $dry = 1 ]; then echo "  + $*"; else "$@"; fi; }
[ $dry = 1 ] && echo "DRY RUN - nothing is changed"
if [ $dry = 0 ] && [ "$(id -u)" != 0 ]; then echo "run it as root (sudo)"; exit 1; fi

if grep -rqs --include='*.conf' 'svr2011/svr-location.conf' /etc/nginx --exclude-dir=svr2011; then
    echo "nginx still includes /etc/nginx/svr2011/svr-location.conf:"
    grep -rns --include='*.conf' 'svr2011/svr-location.conf' /etc/nginx --exclude-dir=svr2011
    echo "take that line out, nginx -t && systemctl reload nginx, then run this again - nothing was changed"
    exit 1
fi

echo "== systemd"
if [ -f /etc/systemd/system/svr2011.service ]; then
    run systemctl disable --now svr2011
    run rm -f /etc/systemd/system/svr2011.service
    run systemctl daemon-reload
else
    echo "  (no unit)"
fi

echo "== files"
run rm -rf /etc/nginx/svr2011 /opt/svr2011
run rm -f /usr/local/sbin/svr2011-admin-password
if [ $purge = 1 ]; then
    run rm -rf /var/lib/svr2011 /var/log/svr2011 /etc/svr2011
    if id svr2011 >/dev/null 2>&1; then run userdel svr2011; fi
else
    echo "  kept: /var/lib/svr2011 (community.db), /var/log/svr2011, /etc/svr2011 (keys), user svr2011 (--purge removes them)"
fi
echo "done"
