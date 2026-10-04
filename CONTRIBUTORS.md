# Contributors

## KaikoClanworth1: project lead

- Started and directs the port.
- Designs the features: the launcher, entrance music and movies, and the extra logos.
- Tests every build and plays it on their own PC.

## gitSothib: contributor

- [Pull request #3](https://github.com/KaikoClanworth1/wwe-svr2011-pc/pull/3), groundwork for peer-to-peer online sessions: the layouts of the game's Xbox LIVE session messages (XGI) and the idea of one session object per session. The port's online matches (`port/src/p2p.cpp`) are built on them.

## Claude Code (Anthropic): AI coding assistant

This port was made with [Claude Code](https://claude.com/claude-code). Claude Code wrote most of this repository's code and docs, working under the project lead's direction:

- **Recompilation setup**: generating the game's code with ReXGlue, and finding the functions and settings the game needs.
- **Runtime fixes**: patches to the ReXGlue runtime (kernel objects, notifications, saves, XMP music) for this game.
- **Graphics**: the native renderer, resolutions and anti-aliasing, and the in-game Graphics page.
- **Sound and input**: the XAudio2 audio backend and controller support.
- **Saves and DLC**: plain-file saves, backups, and installing DLC.
- **Entrance music and movies**: USER PLAYLIST from the Music folder, USER MOVIES from the Custom Movies folder, and the launcher's Bink movie encoder.
- **Created superstars**: up to 10 HD logos, and the Paint Tool import and export.
- **Launcher**: installing the game from the player's own disc image, settings, saves, DLC, Paint Tool and Movies.
- **Tools and docs**: the build, packaging and test scripts, and the README and diagrams.

Commits made with Claude Code end with a `Co-Authored-By: Claude` line.

## ReXGlue and Xenia

The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) is the static recompiler and runtime this port is built on. Its runtime is derived from [Xenia](https://github.com/xenia-project/xenia). This repository keeps only a patch to the SDK (`port/patches/`); see those projects for their contributors and licenses.
