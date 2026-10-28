# Online accounts (2.1.6): invite codes, re-activation, email, password reset

The Community Creations server (`port/server/server.py`, the rules in `signup.py`, the mail in `mailer.py`)
lets only invited players online.

## The rules

| Who | To play online |
|---|---|
| A new player | Signs up in the launcher's (or the Android app's) Online tab with a name, an email, a password and a **one-use invite code** from the server owner, then types the code mailed to that email. |
| An account from before 2.1.6 | Blocked online until, in the 2.1.6 launcher, the player types the **re-activation code** the server owner made for that one account, then adds an email and confirms it. |
| The `--admin-account` account | Needs no re-activation code (it still needs a confirmed email). |

- **Online:** "online" means the game's sign-in (`/api/session`), presence and friends, matches (the relay) and Community Creations. The launcher's own account steps work before that, so an account can still add its email.
- **Codes:**
  - Invite and re-activation codes look like `XXXX-XXXX-XXXX`, without 0/O/1/I. Each works once.
  - A re-activation code works only for its own account, never for a sign-up. Making a new one stops the old one.
- **Mails:** each carries an 8-character code, typed in the launcher.
  - A confirm code lasts 24 hours and its mail also has a one-click link. A reset code lasts 30 minutes.
  - Each code works once, and a newer one replaces the older.
- **Storage:** the server keeps every code and token only as a SHA-256 hash.
- **Sessions:**
  - A password reset ends all of the account's sessions.
  - A password change ends the others but keeps the one that made it.
  - A reset also counts the email as confirmed: the mail reached its owner.
- **Rate limits** (memory only):
  - sign-up: 5 an hour per IP;
  - sign-in: 10 per 10 minutes per IP and per name;
  - confirm: 10 per 30 minutes per account;
  - forgot: 5 an hour per IP, 3 an hour per email;
  - reset: 10 per 30 minutes per email;
  - re-activation: 8 per 30 minutes per account.
- **Forgot password** gives the same answer whether or not the email has an account.
- **Names and emails** are unique in any letter case. Emails are checked for a valid form.
- **Old clients** never fail silently:
  - A 2.1.5 launcher signing in gets the server's text, such as "Online needs the 2.1.6 launcher now: update, then sign in there with the re-activation code the server owner gives you, and add an email."
  - A 2.1.5 game in ONLINE gets the same text in the server's answer, though it shows its own "Sign in…" message.
  - A 2.1.6 game shows the server's reason in the ONLINE dialog.

## The server owner

**Dashboard → Invites:**
- **Make codes:** a note ("who it's for") and how many. They show **once**, so copy them then.
- **Codes:** every sign-up and re-activation code, with who used it and when. Revoke an open one.

**Dashboard → Players, Accounts:**
- **Online access** for each account: blocked (needs its code) / needs an email / email unconfirmed / ok, with the email.
- **Re-activation code:** made for that account and shown once. Give it to that player.
- **Activate:** lets the account in without a code.
- **Set email:** the account gets a confirmation mail. An email set here still has to be confirmed by the player.
- **Resend confirm.**

**On the server's machine:**
- `server.py --data <data> --activate NAME` lets an account in without a code, then exits.

## Mail

- `<data>/mail.json` picks the sender. Without it, mails are `.eml` files in `<data>/mail_out/` (tests, your own server). Examples:

  ```json
  {"service": "brevo",   "from": "SvR2011 Online <no-reply@your-domain>", "api_key_file": "/etc/svr2011/mail_key",
   "public_url": "https://your-domain/svr"}
  {"service": "resend",  "from": "...", "api_key_file": "..."}
  {"service": "mailgun", "from": "...", "api_key_file": "...", "domain": "mg.your-domain", "region": "eu"}
  ```

- **The API key** is in its own file (`api_key_file`), readable by the service's user only. It never goes in `mail.json` or the repo.
- **`public_url`** (or `--public-url`) is the address the confirm link uses. Without it the mail has only the code.
- **Sending** happens on a worker thread. A failure is logged (`mail: ... failed: ...`) and never breaks the request.
- **The sender address's domain** must be verified at the mail service (its SPF/DKIM DNS records), or the mails land in spam.

## Deploying

1. **Before the update:** the user makes the mail service account and its API key, and puts `mail.json` and the key file in place on the server. Then the user gives the go on the VPS update.
2. **Install:** `vps_install.sh` installs `mailer.py` and `signup.py` with the other files.
3. **First start:** the database is backed up to `<data>/backups/community-<time>-before-email.db`. Then the columns are added (`email`, `email_ok`, `activated`) and the tables are made (`invites`, `mail_codes`). No data is removed. From then on every account from before is blocked online until its code and email.
4. **Re-activation:** the server owner makes re-activation codes for the players who keep playing, and invite codes for new ones.

## Tests

- **`port/server/test_signup.py`:** starts its own server on a temporary data folder with a 2.1.5-style database and the file sender, then checks every flow above (50 checks).
- **`--test-open-accounts`:** the old rules (no codes, no email) for private test servers on 127.0.0.1 only. It refuses any other `--host`. The repo's test scripts and `tools/test_server.ps1` start their servers with it.
- **The launcher's Online tab:** `--capture online <file.bmp>` with `SVR2011_CAPTURE_ONLINE=<step>;<step>...`:
  - fields: `name=`, `password=`, `email=`, `code=`;
  - clicks: `click=register|forgot|back|step|resend|signin`;
  - `mode=forgot_code` opens the forgot form's code step.
