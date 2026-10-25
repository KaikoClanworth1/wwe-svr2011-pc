// Running the port's Python converters (port/tools/*.py) from the Mod Maker:
// where the tools and Python are, and a job that runs one in the background
// with its output in the log. The scripts ship next to the exe (Mod Maker
// Tools\) or sit in the source tree (a development build).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mm {

// The folder with the .py tools ("" = not found) and the Python to run them
// ("" = none). Checked once, again after Recheck().
std::wstring ToolsDir();
std::wstring PythonExe();
void RecheckTools();
// "import numpy, PIL" works ("" = yes, else what is missing); cached.
std::string PythonMissing();
// ffmpeg on PATH or C:\ffmpeg\bin ("" = none).
std::wstring FfmpegExe();
// The Mod Maker's own pac tool (WWE '12 - 2K17 pacs) and the Wwise theme decoder, in the Mod Maker Tools folder
// (the dev tree's out\modmaker / third_party in a development build); "" = missing. Every converter gets them
// (--svrmod, SVR2011_SVRMOD / SVR2011_VGMSTREAM), so none can come from somewhere else.
std::wstring SvrmodExe();
std::wstring VgmstreamExe();

// ---- the tools panel ("Mod Maker tools"): Python, numpy, Pillow, the scripts, svrmod.exe, vgmstream, ffmpeg and
// the Bink 2 tool, found or missing in plain words, and "Install what's missing" (the launcher's Setup installs:
// Python + numpy + Pillow and ffmpeg into <game>\Tools from the official sites, pinned and checked, no admin).
void OpenToolsPanel(const std::string& why = "");
void ToolsPanel();          // each frame (app.cpp)
void StartupToolsCheck();   // at start: the panel opens when something the converters need is missing
// Before a Port / convert: true when Python with numpy and Pillow and the scripts are there; else the panel opens
// (with what's missing) and false.
bool ConvertersReady();
// The panel's rows as text (rechecked now): the session log and Report a problem.
std::string ToolsReport();
// Test aids: --tools-panel (open it), --tools-install-from <dir with Tools\>: "Install what's missing" copies from
// there instead of downloading (the same flow after), --tools-install (press it).
void TestToolsPanel(bool install, const std::wstring& from);

// Runs `<python> <tools>\<script> args...` (no window), its output into the
// log line by line, then `done(exit code)` on the UI thread. False (logged)
// when Python or the script is missing or a job already runs.
bool RunTool(const std::wstring& script, const std::vector<std::wstring>& args, std::function<void(int)> done,
             const std::wstring& cwd = L"");
// A converter of the Mod Maker 3 contract (docs/MOD_MAKER_3_PLAN.md section 4):
// stdout is its JSON answer (handed to done with the exit code), stderr goes to
// the log and its "progress <0-100> <text>" lines to the status bar.
bool RunToolJson(const std::wstring& script, const std::vector<std::wstring>& args,
                 std::function<void(int code, const std::string& out)> done, const std::wstring& cwd = L"");
// Move converting is experimental: a RunToolJson that converts moves (moves_port.py convert, superstar_port.py
// convert without --no-moves) first shows a warning the player must accept - once per session, never again with its
// "Don't show this again". Cancel: nothing runs and done() gets exit 2 with {"ok":false,"error":...}. Each frame:
// MovesWarning() draws it. Hidden test runs (--behind) skip it unless SVR2011_TEST_MOVES_WARNING is set
// (show: just shown; accept / accept_dont_show / cancel: answered after a moment, for the test).
void MovesWarning();
bool ConvertsMoves(const std::wstring& script, const std::vector<std::wstring>& args);

// Stops the running RunToolJson converter and everything it started (its job object); its done() then gets a
// non-zero code and ToolCancelled() is true until the next converter starts.
void CancelTool();
bool ToolCancelled();
// Any process the same way (a full command line).
bool RunCommand(const std::wstring& cmdline, std::function<void(int)> done, const std::wstring& cwd = L"");

// A full command line run hidden and waited for (a worker thread: it blocks), stdout and stderr into `out`;
// its exit code (-1: it didn't start). Not a converter job: CancelTool doesn't stop it.
int RunWait(const std::wstring& cmdline, std::string& out);
// An entrance / titantron movie (.bik, .bk2, any video ffmpeg reads) as an .mp4 at `out`: port/tools/movie_shrink.py
// when it is there (its CLI: <in> <out.mp4> --ffmpeg <exe>, the last stdout line its JSON), else ffmpeg itself
// (H.264 CRF 20, yuv420p, AAC, +faststart, the size and rate kept). Blocks; "" when made, else why in plain words.
std::string ShrinkMovie(const std::wstring& in, const std::wstring& out);

// Quotes one argument for a command line.
std::wstring Quote(const std::wstring& arg);

}  // namespace mm
