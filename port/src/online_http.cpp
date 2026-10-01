#include "online_directory.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

namespace svr2011::online {
namespace {
std::wstring Wide(const std::string& s) {
  auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
  if (!n) throw Error(kInvalidArgument, "Invalid UTF-8 configuration");
  std::wstring out(n, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n);
  return out;
}
struct Handle {
  HINTERNET value;
  explicit Handle(HINTERNET v) : value(v) { if (!v) throw Error(kConnectionFailed, "WinHTTP handle creation failed"); }
  ~Handle() { WinHttpCloseHandle(value); }
  Handle(const Handle&) = delete;
};
void Check(BOOL ok) { if (!ok) throw Error(kConnectionFailed, "WinHTTP request failed: " + std::to_string(GetLastError())); }
class Transport final : public HttpTransport {
 public:
  explicit Transport(const std::string& base) {
    auto url = Wide(base); URL_COMPONENTS c{}; c.dwStructSize = sizeof(c);
    c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = c.dwUserNameLength = c.dwPasswordLength = DWORD(-1);
    Check(WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &c));
    if ((c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS) ||
        c.dwUserNameLength || c.dwPasswordLength || c.dwExtraInfoLength || !c.dwHostNameLength)
      throw Error(kInvalidArgument, "Backend must be an HTTP(S) base URL without credentials/query");
    host_.assign(c.lpszHostName, c.dwHostNameLength);
    if (c.dwUrlPathLength) prefix_.assign(c.lpszUrlPath, c.dwUrlPathLength);
    while (!prefix_.empty() && prefix_.back() == L'/') prefix_.pop_back();
    port_ = c.nPort; secure_ = c.nScheme == INTERNET_SCHEME_HTTPS;
  }
  HttpResponse Request(const std::string& method, const std::string& path, const std::string& body) override {
    auto started = GetTickCount64();
    Handle session(WinHttpOpen(L"SvR2011/session-groundwork", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    Check(WinHttpSetTimeouts(session.value, 2000, 2000, 3000, 3000));
    Handle connection(WinHttpConnect(session.value, host_.c_str(), port_, 0));
    auto verb = Wide(method), target = prefix_ + Wide(path);
    Handle request(WinHttpOpenRequest(connection.value, verb.c_str(), target.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    Check(WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)));
    // No certificate-ignore options, embedded credentials or automatic login.
    DWORD login = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    Check(WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &login, sizeof(login)));
    Check(WinHttpSendRequest(request.value, L"Content-Type: application/json\r\n", DWORD(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0));
    Check(WinHttpReceiveResponse(request.value, nullptr));
    HttpResponse result; DWORD status = 0, size = sizeof(status);
    Check(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)); result.status = status;
    for (;;) {
      if (GetTickCount64() - started > 15000) throw Error(kConnectionFailed, "Directory response deadline exceeded");
      DWORD available = 0; Check(WinHttpQueryDataAvailable(request.value, &available)); if (!available) break;
      if (available > 1024 * 1024 - result.body.size()) throw Error(kFailure, "Directory response exceeds 1 MiB");
      auto offset = result.body.size(); result.body.resize(offset + available); DWORD read = 0;
      Check(WinHttpReadData(request.value, result.body.data() + offset, available, &read));
      result.body.resize(offset + read); if (!read) break;
    }
    return result;
  }
 private:
  std::wstring host_, prefix_; INTERNET_PORT port_ = 0; bool secure_ = false;
};
}  // namespace
std::unique_ptr<HttpTransport> MakeWinHttpTransport(const std::string& url) { return std::make_unique<Transport>(url); }
}  // namespace svr2011::online
