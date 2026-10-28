"""Mail for the Community Creations server (server.py): account confirmations and password resets.

One small sender, picked by <data>/mail.json (or --mail-config):

    {"service": "file"}                                    the default: each mail an .eml file in <data>/mail_out/
    {"service": "brevo",   "from": "SvR2011 Online <no-reply@example.com>", "api_key_file": "/etc/svr2011/mail_key"}
    {"service": "resend",  "from": "...", "api_key_file": "..."}
    {"service": "mailgun", "from": "...", "api_key_file": "...", "domain": "mg.example.com", "region": "eu"}

The API key lives in its own file (api_key_file, outside the repo - never in mail.json or the repo); the file
sender needs none (tests, a developer's own server). Mails go out on a worker thread; send() never raises.
Standard library only (urllib).
"""

import base64
import email.message
import email.utils
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

SERVICES = ("file", "brevo", "resend", "mailgun")


class Mailer:
    def __init__(self, config_path, data_dir, log):
        self.log = log
        self.data_dir = data_dir
        self.config = {"service": "file"}
        try:
            with open(config_path, encoding="utf-8") as f:
                self.config.update(json.load(f))
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:
            log("mail: %s unreadable (%s) - mails go to files" % (config_path, e))
        self.service = self.config.get("service", "file")
        if self.service not in SERVICES:
            log("mail: unknown service %r - mails go to files" % self.service)
            self.service = "file"
        self.sender = self.config.get("from") or "SvR2011 Online <no-reply@localhost>"
        self.key = ""
        if self.service != "file":
            try:
                with open(self.config.get("api_key_file", ""), encoding="utf-8") as f:
                    self.key = f.read().strip()
            except OSError as e:
                log("mail: %s's API key unreadable (%s) - mails go to files" % (self.service, e))
                self.service = "file"
        self.out_dir = self.config.get("out_dir") or os.path.join(data_dir, "mail_out")
        log("mail: sent through %s" % (self.service if self.service != "file" else "files in " + self.out_dir))

    def send(self, to, subject, text):
        """Queues one plain-text mail (a worker thread sends it)."""
        threading.Thread(target=self._send, args=(to, subject, text), daemon=True).start()

    def _send(self, to, subject, text):
        try:
            getattr(self, "_" + self.service)(to, subject, text)
            self.log("mail: '%s' sent to %s" % (subject, mask(to)))
        except Exception as e:  # (a mail service down must not take a request with it)
            self.log("mail: '%s' to %s failed: %s" % (subject, mask(to), e))

    def _file(self, to, subject, text):
        msg = email.message.EmailMessage()
        msg["From"], msg["To"], msg["Subject"] = self.sender, to, subject
        msg["Date"] = email.utils.formatdate(localtime=True)
        msg.set_content(text)
        os.makedirs(self.out_dir, exist_ok=True)
        name = "%d-%s.eml" % (int(time.time() * 1000), base64.urlsafe_b64encode(os.urandom(4)).decode().rstrip("="))
        with open(os.path.join(self.out_dir, name), "w", encoding="utf-8") as f:
            f.write(msg.as_string())

    def _post(self, url, data, headers):
        req = urllib.request.Request(url, data=data, method="POST", headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                if r.status >= 300:
                    raise RuntimeError("HTTP %d" % r.status)
        except urllib.error.HTTPError as e:
            raise RuntimeError("HTTP %d: %s" % (e.code, e.read()[:200].decode(errors="replace")))

    def _brevo(self, to, subject, text):
        name, addr = email.utils.parseaddr(self.sender)
        body = {"sender": {"name": name or "SvR2011 Online", "email": addr}, "to": [{"email": to}],
                "subject": subject, "textContent": text}
        self._post("https://api.brevo.com/v3/smtp/email", json.dumps(body).encode(),
                   {"api-key": self.key, "Content-Type": "application/json", "Accept": "application/json"})

    def _resend(self, to, subject, text):
        body = {"from": self.sender, "to": [to], "subject": subject, "text": text}
        self._post("https://api.resend.com/emails", json.dumps(body).encode(),
                   {"Authorization": "Bearer " + self.key, "Content-Type": "application/json"})

    def _mailgun(self, to, subject, text):
        host = "api.eu.mailgun.net" if self.config.get("region") == "eu" else "api.mailgun.net"
        data = urllib.parse.urlencode({"from": self.sender, "to": to, "subject": subject, "text": text}).encode()
        auth = base64.b64encode(("api:" + self.key).encode()).decode()
        self._post("https://%s/v3/%s/messages" % (host, self.config.get("domain", "")), data,
                   {"Authorization": "Basic " + auth, "Content-Type": "application/x-www-form-urlencoded"})


def mask(addr):
    """an address for the log: 'k***@gmail.com'"""
    user, _, domain = (addr or "").partition("@")
    return (user[:1] + "***@" + domain) if domain else "***"
