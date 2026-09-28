# assets/

Game files the build needs. They come from **your own disc** and are never in this repository.

| File | Where it comes from |
|---|---|
| `default.xex` | The game's executable, copied from your disc (or from an install made by the launcher). `rexglue codegen` reads it to generate `generated/`. |
| `svr2011.ico` | Optional. The program icon, made from your game files by `python tools/make_icon.py "<game folder>"`. Without it, the programs build without an icon. |
