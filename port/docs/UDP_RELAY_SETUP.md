# Online relay over UDP: setup and rollback

Online matches between players who can't reach each other directly go
through the server's relay. Until now that relay was https only: TCP, where
one lost segment holds back every later packet, and online matches step only
once every player's input is in - a stall for everyone. The relay now also
speaks UDP (`port/server/relay.py`, `--relay-udp-port`). Games ask for a ticket
over https, register over UDP, and use UDP while it answers. Anything that
goes wrong falls back to the https relay as before, so nothing breaks if a
step below is skipped.

The live server runs on the PC (Project Index, `127.0.0.1:8101`), reached
through the VPS (`sho-ti.me`, nginx) and WireGuard (VPS `10.8.0.1`, PC
`10.8.0.2`). UDP doesn't pass through nginx, so the VPS forwards UDP 36500
down the tunnel itself.

```
player --UDP 36500--> VPS 152.53.32.77 --(firewalld forward + masquerade)--> WireGuard --> PC 10.8.0.2:36500 (server.py)
```

## 1. The PC

1. The server code: the netperf work merged into main (the live server runs
   `port/server` from the main checkout).
2. `F:\ClaudeProjects\Project Index\Apps\SvR2011\service\config.json`: add
   `"relay_udp_port": 36500`. Then in `service.py`, in the `sys.argv` list,
   after the quota line:
   ```python
   *(["--relay-udp-port", str(CONFIG["relay_udp_port"])] if CONFIG.get("relay_udp_port") else []),
   ```
3. Windows firewall, in an administrator PowerShell. This allows UDP 36500 from
   the tunnel network only, never from the internet:
   ```powershell
   New-NetFirewallRule -DisplayName "SvR2011 relay UDP (WireGuard)" -Direction Inbound -Protocol UDP `
       -LocalPort 36500 -RemoteAddress 10.8.0.0/24 -Action Allow
   ```
4. Restart the server: stop the svr2011 service process, then start it again
   (`start-portal.ps1`, or the portal's restart). Its log
   (`Apps\SvR2011\data\server.log`) should then say `relay: UDP on port 36500`.

## 2. The VPS (Fedora 43, firewalld) - run as root

First, record what is there now (for the rollback):
```bash
firewall-cmd --get-default-zone
firewall-cmd --query-masquerade && echo "masquerade was ON" || echo "masquerade was OFF"
sysctl net.ipv4.ip_forward
```

Then forward UDP 36500 down the tunnel:
```bash
firewall-cmd --permanent --add-forward-port=port=36500:proto=udp:toport=36500:toaddr=10.8.0.2
firewall-cmd --permanent --add-masquerade     # (replies must come back through the VPS)
firewall-cmd --reload
sysctl -w net.ipv4.ip_forward=1               # (WireGuard routing usually has this on already)
```

Masquerade means the server sees every relayed player as coming from
`10.8.0.1` (with their own port each). That is fine: the relay tells players
apart by port. Games learn their real public address from STUN anyway.

Check it from any PC with Python, a throwaway account and nothing else:
```bash
python port/server/test_relay.py --server https://sho-ti.me/svr
```
Its `udp:` lines should say `ok`. (It creates three throwaway accounts: delete
them from the dashboard afterwards.)

## Rollback

VPS (as root):
```bash
firewall-cmd --permanent --remove-forward-port=port=36500:proto=udp:toport=36500:toaddr=10.8.0.2
firewall-cmd --permanent --remove-masquerade   # ONLY if it was OFF before
firewall-cmd --reload
```
PC: remove `relay_udp_port` from `config.json` (or set it to 0) and restart the
server. Optionally remove the firewall rule:
`Remove-NetFirewallRule -DisplayName "SvR2011 relay UDP (WireGuard)"`. Games
then use the https relay again, with no other change.

## Match network switch (dashboard)

Dashboard -> Settings -> **match network** (server setting `match_network`, saved in data/settings.json,
logged as `settings: match_network=... (dashboard)`):

- **auto** (default): matches connect directly; the relay only when direct fails.
- **relay**: every match through the relay.
- **p2p**: match traffic direct only. The relay still carries the small search / hello messages that let
  internet games find each other (there is no other session list), never match data. For when the
  server struggles; a pair that cannot connect directly then fails to join.

Games read it with the presence poll (`/api/friends` config, every 20 s; no server: auto). The host puts
its mode in its session (property 0x1000F004) and joiners follow it, so both use the same; a running
match keeps its mode. Logs: game `p2p: match network <mode> (host|joiner ...)`, the match report and the
server `netreport: ... match network <mode>` line; the Live view tags each session with its mode.
