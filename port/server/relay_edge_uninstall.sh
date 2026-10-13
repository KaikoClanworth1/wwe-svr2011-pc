#!/bin/bash
# Removes the relay edge from the VPS and puts the UDP 36500 forward to the PC
# back (RELAY_EDGE.md, "Going back"). Run as root: sudo bash relay_edge_uninstall.sh
# Then take relay_edge out of the PC server's config and restart it.
set -uo pipefail
fwd='port=36500:proto=udp:toport=36500:toaddr=10.8.0.2'
link_rule='rule family="ipv4" source address="10.8.0.0/24" port port="36501" protocol="udp" accept'

systemctl disable --now svr-relay-edge
rm -f /etc/systemd/system/svr-relay-edge.service
systemctl daemon-reload

zone=$(firewall-cmd --get-default-zone)
wgzone=$(firewall-cmd --get-zone-of-interface=wg0 2>/dev/null || echo "$zone")
firewall-cmd --permanent --zone="$wgzone" --remove-rich-rule="$link_rule"
firewall-cmd --permanent --zone="$zone" --remove-port=36500/udp
firewall-cmd --permanent --zone="$zone" --add-forward-port="$fwd"
firewall-cmd --reload
echo "done - the edge is gone and UDP 36500 goes to the PC again (/opt/svr-relay is kept)"
