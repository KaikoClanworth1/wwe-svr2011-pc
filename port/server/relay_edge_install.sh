#!/bin/bash
# Installs the relay edge (relay_edge.py) on the VPS - see RELAY_EDGE.md.
# Run as root from a folder holding relay_edge.py, relay_link.py and secret.txt
# (the same secret as the PC server's relay_secret.txt):
#   sudo bash relay_edge_install.sh
# Undo: sudo bash relay_edge_uninstall.sh
# It never touches the WireGuard tunnel (no wg / wg-quick, no reload, masquerade kept).
set -euo pipefail
src="$(cd "$(dirname "$0")" && pwd)"
fwd='port=36500:proto=udp:toport=36500:toaddr=10.8.0.2'
link_rule='rule family="ipv4" source address="10.8.0.0/24" port port="36501" protocol="udp" accept'

for f in relay_edge.py relay_link.py secret.txt; do
    [ -f "$src/$f" ] || { echo "missing $src/$f"; exit 1; }
done
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' || { echo "python3 3.8 or newer is needed"; exit 1; }
if ! ip -4 addr show wg0 | grep -q 'inet 10\.8\.0\.1/'; then
    echo "wg0 is not 10.8.0.1 on this machine - nothing was changed"; ip -4 addr show wg0 || true; exit 1
fi

echo "== files: /opt/svr-relay"
install -d -m 755 /opt/svr-relay
install -m 644 "$src/relay_edge.py" "$src/relay_link.py" /opt/svr-relay/
install -m 600 "$src/secret.txt" /opt/svr-relay/secret.txt
restorecon -R /opt/svr-relay 2>/dev/null || true

echo "== service: svr-relay-edge"
cat > /etc/systemd/system/svr-relay-edge.service <<'EOF'
[Unit]
Description=SvR2011 online relay edge
After=network-online.target wg-quick@wg0.service
Wants=network-online.target

[Service]
LoadCredential=secret:/opt/svr-relay/secret.txt
ExecStart=/usr/bin/python3 /opt/svr-relay/relay_edge.py --udp-port 36500 --link 10.8.0.1:36501 --secret %d/secret
DynamicUser=yes
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now svr-relay-edge

echo "== firewall"
zone=$(firewall-cmd --get-default-zone)
wgzone=$(firewall-cmd --get-zone-of-interface=wg0 2>/dev/null || echo "$zone")
echo "default zone $zone, wg0 zone $wgzone"
# each change now and saved, with no --reload: a reload would drop runtime-only
# rules something else (the WireGuard setup) may rely on. Nothing else is touched:
# no wg / wg-quick, the masquerade stays.
fw() { firewall-cmd "$@" && firewall-cmd --permanent "$@"; }
fw --zone="$zone" --remove-forward-port="$fwd" || true
fw --zone="$zone" --add-port=36500/udp
fw --zone="$wgzone" --add-rich-rule="$link_rule"

sleep 2
echo "== check"
systemctl --no-pager --lines=5 status svr-relay-edge || true
ss -ulpn | grep -E ':3650[01] ' || echo "nothing listens on 36500/36501 yet"
echo "done - tell Claude, it restarts the PC server with the edge"
