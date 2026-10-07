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

// Runs `<python> <tools>\<script> args...` (no window), its output into the
// log line by line, then `done(exit code)` on the UI thread. False (logged)
// when Python or the script is missing or a job already runs.
bool RunTool(const std::wstring& script, const std::vector<std::wstring>& args, std::function<void(int)> done,
             const std::wstring& cwd = L"");
// Any process the same way (a full command line).
bool RunCommand(const std::wstring& cmdline, std::function<void(int)> done, const std::wstring& cwd = L"");

// Quotes one argument for a command line.
std::wstring Quote(const std::wstring& arg);

}  // namespace mm
