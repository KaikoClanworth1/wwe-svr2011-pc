// Python converters run from the Mod Maker (tool_run.h).
#include "tool_run.h"

#include "app.h"

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
  // Python: the launcher (py), then python on PATH - not the Store stub
  for (const wchar_t* name : {L"python.exe", L"python3.exe"}) {
    const std::wstring p = OnPath(name);
    if (!p.empty() && p.find(L"WindowsApps") == std::wstring::npos) { g_python = p; break; }
  }
  if (g_python.empty() && !OnPath(L"py.exe").empty()) g_python = OnPath(L"py.exe");
  g_ffmpeg = OnPath(L"ffmpeg.exe");
  if (g_ffmpeg.empty() && fs::exists(L"C:\\ffmpeg\\bin\\ffmpeg.exe", ec)) g_ffmpeg = L"C:\\ffmpeg\\bin\\ffmpeg.exe";
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

}  // namespace

std::wstring ToolsDir() { Check(); return g_tools; }
std::wstring PythonExe() { Check(); return g_python; }
std::wstring FfmpegExe() { Check(); return g_ffmpeg; }
void RecheckTools() { g_checked = false, g_missing_checked = false; }

std::string PythonMissing() {
  Check();
  if (g_missing_checked) return g_missing;
  g_missing_checked = true;
  if (g_python.empty()) return g_missing = "Python 3 (python.org; tick 'Add to PATH')";
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

bool RunTool(const std::wstring& script, const std::vector<std::wstring>& args, std::function<void(int)> done,
             const std::wstring& cwd) {
  Check();
  if (g_python.empty()) {
    Status("Python 3 is needed for this (python.org; tick 'Add to PATH'), then Recheck.");
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
