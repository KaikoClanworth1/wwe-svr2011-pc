#include "online_sessions.h"
#include "online_directory.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <random>

REXCVAR_DEFINE_BOOL(p2p_sessions, false, "Online", "Experimental session groundwork; restart required").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(p2p_api, "", "Online", "Existing Xenia-WebServices base URL").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(p2p_xuid, "", "Online", "Explicit unique 16-hex session identity (user zero)").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(p2p_name, "Player", "Online", "Session directory name").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(p2p_host_address, "", "Online", "Reachable IPv4 address; must match directory source address for DELETE").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(p2p_mac, "", "Online", "Explicit unique 12-hex peer MAC").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(p2p_port, 36000, "Online", "Advertised peer port (transport not implemented)").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace svr2011::online {
namespace {
class SessionObject final : public rex::system::XObject {
 public:
  static constexpr Type kObjectType = Type::Session;
  SessionObject(rex::system::KernelState* k, std::shared_ptr<SessionService> service)
      : XObject(k, kObjectType), service_(std::move(service)) {}
  void Initialize() {
    auto* value = CreateNative<rex::be<uint32_t>>();
    if (!value) throw Error(kFailure, "Session object allocation failed");
    *value = handle(); service_->Reserve(handle()); reserved_handle_ = handle();
  }
  ~SessionObject() override { if (reserved_handle_) service_->Release(reserved_handle_); }
 private:
  std::shared_ptr<SessionService> service_; uint32_t reserved_handle_ = 0;
};
class Memory final : public GuestMemory {
 public:
  explicit Memory(rex::system::KernelState* kernel) : kernel_(kernel) {}
  std::span<uint8_t> Map(uint32_t address, size_t size, bool write) override {
    auto* memory = kernel_->memory();
    auto end = uint64_t(address) + size;
    if (!address || !size || size > 1024 * 1024 || end > uint64_t(UINT32_MAX) + 1)
      throw Error(kInvalidArgument, "Invalid guest span");
    auto* heap = memory->LookupHeap(address);
    if (!heap || heap != memory->LookupHeap(uint32_t(end - 1)))
      throw Error(kInvalidArgument, "Guest span crosses heap");
    // QueryRegionInfo starts at the containing page; visit every page rather
    // than assume an allocation's whole range has one protection/state.
    for (uint64_t cursor = address; cursor < end;) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(cursor), &info) ||
          !(info.state & rex::memory::kMemoryAllocationCommit) ||
          !(info.protect & rex::memory::kMemoryProtectRead) ||
          (write && !(info.protect & rex::memory::kMemoryProtectWrite)))
        throw Error(kInvalidArgument, "Uncommitted or protected guest memory");
      // 4 KiB is no larger than any Xbox heap page, so cannot skip a region.
      cursor = (cursor & ~uint64_t(4095)) + 4096;
    }
    return {memory->TranslateVirtual(address), size};
  }
  uint32_t SessionHandle(uint32_t address) override {
    auto object = rex::system::XObject::GetCreatedNativeObject(address);
    if (!object || object->kernel_state() != kernel_ || object->type() != SessionObject::kObjectType || object->handles().empty())
      throw Error(kInvalidHandle, "Not a session object");
    return object->handle();
  }
 private:
  rex::system::KernelState* kernel_;
};
class Provider final : public rex::system::xam::AppManager::SessionProvider {
 public:
  Provider(rex::system::KernelState* k, Identity i, const std::string& url)
      : kernel_(k), http_(MakeWinHttpTransport(url)), directory_(*http_),
        service_(std::make_shared<SessionService>(directory_, std::move(i), [] {
          // Session IDs/nonces are identifiers, not authentication secrets.
          std::random_device rng; return (uint64_t(rng()) << 32) | rng();
        })), memory_(k), bridge_(*service_, memory_) {}
  uint32_t CreateHandle(uint32_t* handle) override {
    try {
      auto object = rex::system::make_object<SessionObject>(kernel_, service_);
      try { object->Initialize(); } catch (...) { object->ReleaseHandle(); throw; }
      *handle = object->handle(); return kSuccess;
    } catch (const Error& e) { return e.code; } catch (...) { return kFailure; }
  }
  uint32_t ReferenceObject(uint32_t handle, uint32_t* address) override {
    auto object = kernel_->object_table()->LookupObject<SessionObject>(handle);
    if (!object) return kInvalidHandle;
    // SDK ObDereferenceObject releases the retained handle, as it already
    // does for native enumerators. This survives the wrapper's CloseHandle.
    object->RetainHandle(); *address = object->guest_object(); return kSuccess;
  }
  std::optional<uint32_t> Dispatch(uint32_t message, uint32_t address, uint32_t length) override {
    auto result = bridge_.Dispatch(message, address, length);
    if (result) REXLOG_DEBUG("P2P XGI {:08X} -> {}", message, *result);
    return result;
  }
 private:
  rex::system::KernelState* kernel_;
  std::unique_ptr<HttpTransport> http_;
  RestDirectory directory_;
  std::shared_ptr<SessionService> service_;
  Memory memory_; GuestBridge bridge_;
};
}  // namespace
void InstallSessions(rex::system::KernelState* kernel) {
  if (!REXCVAR_GET(p2p_sessions)) return;
  try {
    Identity i;
    i.xuid = i.machine_id = ParseHex(REXCVAR_GET(p2p_xuid));
    i.name = REXCVAR_GET(p2p_name); i.address = REXCVAR_GET(p2p_host_address); i.mac = REXCVAR_GET(p2p_mac);
    auto port = REXCVAR_GET(p2p_port);
    if (!port || port > 65535) throw Error(kInvalidArgument, "Invalid advertised port");
    i.port = uint16_t(port);
    if (REXCVAR_GET(p2p_api).empty()) throw Error(kInvalidArgument, "Explicit existing directory URL required");
    kernel->app_manager()->SetSessionProvider(std::make_shared<Provider>(kernel, std::move(i), REXCVAR_GET(p2p_api)));
    REXLOG_INFO("Experimental P2P session provider installed (no match transport)");
  } catch (const std::exception& e) {
    REXLOG_ERROR("P2P session configuration rejected: {}", e.what());
    // Fail startup instead of silently activating the old success-only stubs.
    throw;
  }
}
}  // namespace svr2011::online
