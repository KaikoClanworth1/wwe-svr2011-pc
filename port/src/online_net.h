// WWE SmackDown vs. Raw 2011 - the port's connection to the Community
// Creations server (online_server, a URL such as https://sho-ti.me/svr).
//
// The game speaks GameSpy: plain http to fixed paths and a presence (GP)
// login on raw TCP ports. Neither can go to a web address, so the game's
// GameSpy connections come to a relay here on this PC (the runtime's
// connect() sends them, see online_relay_http / online_relay_gp):
//   - http requests go on to <online_server>/game/<path> over HTTP(S), with
//     the player's account (online_token, from signing in in the launcher);
//   - the presence login is answered here: its profile and login ticket come
//     from <online_server>/api/session.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace svr2011::net {

struct Response {
  int status = 0;
  std::map<std::string, std::string> headers;  // (lower-case names)
  std::string body;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

// A request to any http(s) URL (https: Windows only for now); nullopt if it
// didn't get an answer.
std::optional<Response> Request(const std::string& method, const std::string& url, const Headers& headers = {},
                                const std::string& body = {});

// A request to the server: `path` under online_server (e.g. "/api/session"),
// with the account's token.
std::optional<Response> ServerRequest(const std::string& method, const std::string& path,
                                      const std::string& body = {}, Headers headers = {});

// Starts the relay (once) and tells the runtime its ports.
bool StartRelay();

// Community Creations files the relay passed on (entrance_media.cpp): an
// upload the server took as `fileid` (body: the game's multipart form), or
// a download of `fileid` (body: the file). Called on the relay's thread.
using TransferListener = void (*)(bool upload, int fileid, const std::string& content_type, const std::string& body);
void SetTransferListener(TransferListener listener);

}  // namespace svr2011::net
