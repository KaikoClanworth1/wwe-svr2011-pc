#!/bin/bash
# One command for vps_cutover.md section A on the VPS (run once, as root):
#   sudo bash ~/svr2011-src/port/server/vps_prepare.sh
# Installs the core (unit DISABLED, nginx location not included), asks for the
# dashboard password, and prints the storage key for the PC. Nothing goes live.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }

bash "$here/vps_install.sh"
echo
echo "== the dashboard password (https://sho-ti.me/svr-admin/) - at least 10 characters, a new one"
if [ -s /var/lib/svr2011/admin_password ]; then
    read -r -p "A password is set already. Change it? [y/N] " a
    [ "${a:-n}" = y ] && svr2011-admin-password
else
    svr2011-admin-password
fi
echo
echo "== the storage key: copy it for the PC (pc_storage_setup.ps1 asks for it). It is a secret."
cat /etc/svr2011/storage_key
echo
echo "Done. The service is installed but OFF until the switch (vps_switch.sh)."
