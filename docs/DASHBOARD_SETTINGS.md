# Dashboard: Settings tab

## Storage

Where the server keeps its big files. Each folder can be changed on its own:

| Folder | What is in it | Where it lives |
| --- | --- | --- |
| Community Creations, Discovery mods, entrance songs and movies | every uploaded file, stored once by its content | the storage node (PC) when the VPS runs as `--role core`; else this server |
| Problem reports and suggestions | reports, their pictures, match folders and the mods sent with them | the storage node (PC) when there is one; else this server |
| Account database backups | copies of `community.db` made before account changes | always the server running the dashboard (the VPS) |

Community Creations, Discovery and the entrance media share one folder: a file is stored once by its content, so
the same file can belong to more than one of them.

`community.db`, `settings.json`, `storage.json`, the name lists and the secrets stay in the `--data` folder.

For each folder the page shows its path, the space it uses, the free space on its disk and the default path.

**To move a folder:**

1. Type the new folder (a full path) and press **Check**. The check makes sure that:
   - the folder can be made or already exists, and is empty (or is one of this folder's old copies);
   - the server can write there;
   - it isn't inside another storage folder, and no storage folder is inside it;
   - the disk has room for the files plus 512 MB.
2. Press **Move files**. Every file is copied, with progress shown. Then, with that folder's writes held for a
   moment, files that changed meanwhile are copied again and every file's size is checked. Only after that does the
   server switch to the new folder.
3. If anything fails, the server keeps using the old folder and nothing is lost.
4. The old folder is kept and listed as **old copy**. **Delete old copy** shows its size and needs you to type
   `DELETE`. Nothing is ever deleted on its own.

The choice is saved in `<data>/storage.json` and stays after a restart. On the storage node it is saved in the
node's own `--data` folder. The VPS dashboard changes the node's folders through the node's key
(`/__storage` on the node). A node running an older server says so: update its files first.

Files: `port/server/storage.py`. Tests: `py -3.11 test_server.py --storage-only`, which needs no server; on a
private `--local-admin` server, `test_server.py --server ... --admin`.

## Settings

The rows are grouped into Online, Accounts, Reports, Discovery, Community Creations and Supporters.

- Every row shows its default.
- Rows tagged **restart** are saved at once but used from the next start.
- These rows replace command-line options: `max_file_mb`, `quota_mb`, `max_music_mb`, `max_movie_mb`,
  `media_quota_mb`, `report_mod_quota_gb` and `patreon_campaign`. Once one is saved on the page, the saved value
  wins over the option.
- The limits that used to be fixed in the code are now rows: report rate limits, report mod checks, sign-in tries,
  the largest Discovery mod and the hours between Patreon checks.

**Secrets** (Ko-fi token, Patreon token):

- The page only shows whether each one is set, and where from.
- **Replace** writes the new value to `<data>/secrets/<name>` (mode 600). That copy is read before the file given on
  the command line.
- **Use the file** removes the page's copy, so the command-line file is used again.

**Server (command line)** lists what the service file sets: role, data folder, ports, storage node, relay,
dashboard sign-in, log and supervisor. Those can't be changed on the page.
