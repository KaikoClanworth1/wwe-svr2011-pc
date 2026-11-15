# Supporters list: setup

The launcher's **Supporters** tab lists the people who support the project. Supporting is optional and unlocks
nothing; the tab says so at the top. The online server fills the list from three places:

- **Ko-fi**: Ko-fi tells the server about each payment (a "webhook").
- **Patreon**: the server checks your active patrons every 6 hours.
- **By hand**: you add PayPal donors (or anyone else) who ask to be listed, on the dashboard's Supporters tab.

Only names are kept, and only names people chose to show:

- A Ko-fi payment the supporter marked private is never listed.
- A patron who stops is taken off the list.
- No email, amount, message or tier is ever stored or shown.

Each part works on its own. If a token file is missing, that source is simply off.

## 1. Ko-fi

1. On Ko-fi, open **Settings → More → API** (Ko-fi calls it "Webhooks").
2. In **Webhook URL**, paste: `https://sho-ti.me/svr/api/supporters/kofi`
3. Ko-fi shows a **Verification Token** on the same page. Copy it.
4. On the server, put the token in a file only the server can read:

   ```
   sudo sh -c 'printf "%s\n" "PASTE-THE-TOKEN-HERE" > /etc/svr2011/kofi_token'
   sudo chown root:svr2011 /etc/svr2011/kofi_token
   sudo chmod 640 /etc/svr2011/kofi_token
   ```

   The server reads this file on each Ko-fi call, so a new token works without a restart.
5. Back on Ko-fi, press **Send Test**. The dashboard's Supporters tab should show Ko-fi's test name (e.g.
   "Jo Example"). Press **Hide** on it so it never shows in the launcher.

## 2. Patreon

1. Open https://www.patreon.com/portal/registration/register-clients and press **Create Client**.
   - App name: anything, e.g. "SvR2011 supporters list".
   - Redirect URI: anything, e.g. `https://sho-ti.me/`. The server doesn't use it.
   - Client API version: **2**.
2. Open the new client and copy the **Creator's Access Token**.
3. On the server:

   ```
   sudo sh -c 'printf "%s\n" "PASTE-THE-TOKEN-HERE" > /etc/svr2011/patreon_token'
   sudo chown root:svr2011 /etc/svr2011/patreon_token
   sudo chmod 640 /etc/svr2011/patreon_token
   ```

4. On the dashboard's Supporters tab, press **Check Patreon now**. The line at the top shows the result, e.g.
   "12 active patron(s), 12 new".
   - If it says "Patreon said HTTP 401", the token was refused or has expired. Copy a fresh one from the same page
     into the file and press the button again.
   - The server uses your first campaign. If you have more than one, start it with `--patreon-campaign <id>`.

## 3. PayPal and others: by hand

On the dashboard's **Supporters** tab, type the name as it should show, pick PayPal or Other, and press **Add**.
**Remove** takes off a name added by hand. **Hide** works on any name; a hidden name stays hidden whatever comes in
later. **Show** brings a hidden name back.

## What the launcher shows

`GET /svr/api/supporters` returns the names, the platform (Ko-fi, Patreon, PayPal, Other) and the date each one
started, newest first. It holds at most 500 names and is cached for a minute.

## Notes for the server

- Code: `port/server/supporters.py`. Tests: `port/server/test_supporters.py` uses fake Ko-fi calls and a mock
  Patreon, and contacts neither service.
- Flags: `--kofi-token-file` (default `/etc/svr2011/kofi_token`), `--patreon-token-file` (default
  `/etc/svr2011/patreon_token`) and `--patreon-campaign`.
- The tokens are never logged, never stored in the database and never sent anywhere except to Patreon itself (the
  Patreon token only).
- Android doesn't have the tab yet. A note in `SuggestionsList.java` shows where the same endpoint would plug in.
