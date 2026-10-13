// Python converters run from the Mod Maker (tool_run.h).
#include "tool_run.h"

#include "app.h"

#include <thread>

namespace mm {

namespace {

std::wstring g_tools, g_python, g_ffmpeg;
bool g_checked = false;
std::string g_missing;
bool g_missing_checked = false;

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(buf).parent_path().wstring();
}

// A Microsoft Store app alias (the 0-byte python.exe that opens the Store) - a real Store Python isn't one.
bool IsAlias(const std::wstring& p) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  return !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.nFileSizeHigh == 0 && a.nFileSizeLow == 0);
}

// The launcher's First time setup puts Python (numpy, Pillow) and ffmpeg in <game>\Tools.
std::vector<fs::path> ToolsHomes() {
  std::vector<fs::path> out = {fs::path(ExeDir()) / L"Tools"};
  if (!g_game.empty()) out.push_back(fs::path(g_game) / L"Tools");
  return out;
}

// The first program on PATH with that name ("" = none).
std::wstring OnPath(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  if (SearchPathW(nullptr, name, L".exe", DWORD(std::size(buf)), buf, nullptr)) return buf;
  return L"";
}

void Check() {
  if (g_checked) return;
  g_checked = true;
  const std::wstring exe = ExeDir();
  std::error_code ec;
  for (const fs::path cand : {fs::path(exe) / L"Mod Maker Tools", fs::path(exe) / L"tools",
                              fs::path(exe) / L".." / L".." / L".." / L"tools",  // (out/build/<preset> -> port/tools)
                              fs::path(exe) / L".." / L"tools"})
    if (fs::exists(cand / L"svr10_superstar.py", ec)) {
      g_tools = fs::weakly_canonical(cand, ec).wstring();
      break;
    }
  // Python: the launcher setup's (<game>\Tools\python), then python / python3 on PATH (not the Store's
  // 0-byte alias), then the py launcher
  for (const auto& home : ToolsHomes())
    if (g_python.empty() && fs::exists(home / L"python" / L"python.exe", ec)) g_python = (home / L"python" / L"python.exe").wstring();
  for (const wchar_t* name : {L"python.exe", L"python3.exe"}) {
    if (!g_python.empty()) break;
    const std::wstring p = OnPath(name);
    if (!p.empty() && !IsAlias(p)) g_python = p;
  }
  if (g_python.empty() && !OnPath(L"py.exe").empty()) g_python = OnPath(L"py.exe");
  // ffmpeg: the setup's, then PATH, then C:\ffmpeg\bin; the setup's goes first on the scripts' PATH too
  // (their shutil.which('ffmpeg') finds it: children inherit this process's environment)
  for (const auto& home : ToolsHomes())
    if (g_ffmpeg.empty() && fs::exists(home / L"ffmpeg" / L"ffmpeg.exe", ec)) {
      g_ffmpeg = (home / L"ffmpeg" / L"ffmpeg.exe").wstring();
      wchar_t path[32767];
      const DWORD n = GetEnvironmentVariableW(L"PATH", path, DWORD(std::size(path)));
      const std::wstring dir = (home / L"ffmpeg").wstring();
      if (n < std::size(path) && std::wstring(path, n).find(dir) == std::wstring::npos)
        SetEnvironmentVariableW(L"PATH", (dir + L";" + std::wstring(path, n)).c_str());
    }
  if (g_ffmpeg.empty()) g_ffmpeg = OnPath(L"ffmpeg.exe");
  if (g_ffmpeg.empty() && fs::exists(L"C:\\ffmpeg\\bin\\ffmpeg.exe", ec)) g_ffmpeg = L"C:\\ffmpeg\\bin\\ffmpeg.exe";
  Log("tools: scripts " + (g_tools.empty() ? std::string("not found") : Utf8(g_tools)) + ", Python " +
      (g_python.empty() ? std::string("not found") : Utf8(g_python)) + ", ffmpeg " +
      (g_ffmpeg.empty() ? std::string("not found") : Utf8(g_ffmpeg)));
}

// Runs a command with no window, reading its output into the log; returns the exit code.
int RunCapture(const std::wstring& cmdline, const std::wstring& cwd) {
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = cmdline;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                 nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    Log("Could not start: " + Utf8(cmdline));
    return -1;
  }
  std::string line;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
    for (DWORD i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) Log("  " + line), Progress(line.substr(0, 120));
        line.clear();
      } else {
        line += buf[i];
      }
    }
  }
  if (!line.empty()) Log("  " + line);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

// stdout collected, stderr logged on a thread of its own (progress lines to the bar).
int RunCaptureJson(const std::wstring& cmdline, const std::wstring& cwd, std::string& out) {
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE ord = nullptr, owr = nullptr, erd = nullptr, ewr = nullptr;
  if (!CreatePipe(&ord, &owr, &sa, 0)) return -1;
  if (!CreatePipe(&erd, &ewr, &sa, 0)) {
    CloseHandle(ord), CloseHandle(owr);
    return -1;
  }
  SetHandleInformation(ord, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(erd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = owr;
  si.hStdError = ewr;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = cmdline;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                 nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(owr);
  CloseHandle(ewr);
  if (!ok) {
    CloseHandle(ord), CloseHandle(erd);
    Log("Could not start: " + Utf8(cmdline));
    return -1;
  }
  std::thread err([erd] {
    std::string line;
    char buf[4096];
    DWORD n = 0;
    auto flush = [&] {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("progress ", 0) == 0) {
        const size_t sp = line.find(' ', 9);
        Progress(sp == std::string::npos ? line.substr(9) : line.substr(9, sp - 9) + "%  " + line.substr(sp + 1, 110));
      } else if (!line.empty()) {
        Log("  " + line);
      }
      line.clear();
    };
    while (ReadFile(erd, buf, sizeof buf, &n, nullptr) && n)
      for (DWORD i = 0; i < n; ++i)
        if (buf[i] == '\n') flush();
        else line += buf[i];
    flush();
    CloseHandle(erd);
  });
  char buf[8192];
  DWORD n = 0;
  while (ReadFile(ord, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
  CloseHandle(ord);
  err.join();
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

}  // namespace

std::wstring ToolsDir() { Check(); return g_tools; }
std::wstring PythonExe() { Check(); return g_python; }
std::wstring FfmpegExe() { Check(); return g_ffmpeg; }
void RecheckTools() { g_checked = false, g_missing_checked = false; }

std::string PythonMissing() {
  Check();
  if (g_missing_checked) return g_missing;
  g_missing_checked = true;
  if (g_python.empty()) return g_missing = "Python 3 - install the Mod Maker tools from the launcher's Setup page";
  // a quick import check, no window
  const std::wstring probe = Quote(g_python) + L" -c \"import numpy, PIL; print('ok')\"";
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  CreatePipe(&rd, &wr, &sa, 0);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr, si.hStdError = wr;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = probe;
  std::string out;
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(wr);
    char buf[512];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
  } else {
    CloseHandle(wr);
  }
  CloseHandle(rd);
  if (out.find("ok") == std::string::npos) g_missing = "the Python packages numpy and Pillow (pip install numpy pillow)";
  return g_missing;
}

std::wstring Quote(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring q = L"\"";
  for (wchar_t c : arg) q += c == L'"' ? L"\\\"" : std::wstring(1, c);
  return q + L"\"";
}

bool RunCommand(const std::wstring& cmdline, std::function<void(int)> done, const std::wstring& cwd) {
  if (Busy()) {
    Status("Still working on the last job: wait a moment.");
    return false;
  }
  Log("> " + Utf8(cmdline));
  RunInBackground([cmdline, cwd, done] {
    Progress("Running ...");
    const int code = RunCapture(cmdline, cwd);
    OnUiThread([done, code] { if (done) done(code); });
  });
  return true;
}

bool RunToolJson(const std::wstring& script, const std::vector<std::wstring>& args,
                 std::function<void(int, const std::string&)> done, const std::wstring& cwd) {
  Check();
  if (g_python.empty()) {
    Status("Python 3 is needed for this: install the Mod Maker tools from the launcher's Setup page, then Recheck.");
    return false;
  }
  if (g_tools.empty() || !fs::exists(fs::path(g_tools) / script)) {
    Status("The converter " + Utf8(script) + " isn't in the Mod Maker Tools folder.");
    return false;
  }
  if (Busy()) {
    Status("Still working on the last job: wait a moment.");
    return false;
  }
  std::wstring cmd = Quote(g_python) + L" -X utf8 " + Quote((fs::path(g_tools) / script).wstring());
  for (const auto& a : args) cmd += L" " + Quote(a);
  std::wstring work = cwd;  // (as RunTool: what a script drops lands in Mods\.convert)
  if (work.empty()) {
    const fs::path w = fs::path(g_game) / L"Mods" / L".convert";
    std::error_code ec;
    fs::create_directories(w, ec);
    work = w.wstring();
  }
  Log("> " + Utf8(cmd));
  RunInBackground([cmd, cwd = work, done] {
    Progress("Running ...");
    std::string out;
    const int code = RunCaptureJson(cmd, cwd, out);
    OnUiThread([done, code, out] { if (done) done(code, out); });
  });
  return true;
}

bool RunTool(const std::wstring& script, const std::vector<std::wstring>& args, std::function<void(int)> done,
             const std::wstring& cwd) {
  Check();
  if (g_python.empty()) {
    Status("Python 3 is needed for this: install the Mod Maker tools from the launcher's Setup page, then Recheck.");
    return false;
  }
  if (g_tools.empty()) {
    Status("The converter scripts (Mod Maker Tools folder next to the Mod Maker) were not found.");
    return false;
  }
  std::wstring cmd = Quote(g_python) + L" " + Quote((fs::path(g_tools) / script).wstring());
  for (const auto& a : args) cmd += L" " + Quote(a);
  // the tools' working folder: whatever a script drops (surveys, caches) lands here, not in the tools folder
  std::wstring work = cwd;
  if (work.empty()) {
    const fs::path w = fs::path(g_game) / L"Mods" / L".convert";
    std::error_code ec;
    fs::create_directories(w, ec);
    work = w.wstring();
  }
  return RunCommand(cmd, std::move(done), work);
}

}  // namespace mm
