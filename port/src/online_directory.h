#pragma once
#include "online_sessions.h"
#include <memory>

namespace svr2011::online {
struct HttpResponse { unsigned status = 0; std::string body; };
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  // Implementations must enforce timeouts, response limits and normal TLS
  // validation. Paths are relative to the configured existing backend.
  virtual HttpResponse Request(const std::string& method,
                               const std::string& path,
                               const std::string& body) = 0;
};

// Xenia-WebServices contract referenced by docs/ONLINE_PLAN.md. The service
// owns no HTTP details, making a different existing backend easy to adapt.
class RestDirectory final : public Directory {
 public:
  explicit RestDirectory(HttpTransport& http) : http_(http) {}
  void Register(const Identity&) override;
  void Create(const Session&) override;
  std::vector<Session> Find(const Search&) override;
  Session Get(uint64_t id) override;
  void Join(uint64_t id, const std::vector<Member>&) override;
  void Leave(uint64_t id, const std::vector<uint64_t>&) override;
  void Delete(uint64_t id) override;
 private:
  std::string Call(const std::string&, const std::string&, const std::string& = "");
  std::string Path(uint64_t id) const;
  HttpTransport& http_;
  uint64_t xuid_ = 0;
};
std::string EncodeProperty(const Property&);
Property DecodeProperty(const std::string&);
// Windows host transport; separate from the portable REST/session service.
std::unique_ptr<HttpTransport> MakeWinHttpTransport(const std::string& base_url);
}  // namespace svr2011::online
