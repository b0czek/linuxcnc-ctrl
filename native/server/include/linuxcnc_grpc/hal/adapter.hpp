#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace linuxcnc::server {

enum class HalAdapterType { Bit, Float, S32, U32, S64, U64 };
using HalAdapterValue = std::variant<bool, double, std::int32_t, std::uint32_t,
                                     std::int64_t, std::uint64_t>;

enum class HalAdapterItemKind { Pin, Param, Signal };
enum class HalAdapterPinDirection { In, Out, Io };
enum class HalAdapterParamDirection { ReadOnly, ReadWrite };
enum class HalAdapterComponentKind { Unknown, User, Realtime, Other };

struct HalAdapterReference {
  HalAdapterItemKind kind = HalAdapterItemKind::Pin;
  std::string name;
};

struct HalAdapterComponentInfo {
  int id = 0;
  std::string name;
  HalAdapterComponentKind kind = HalAdapterComponentKind::Unknown;
  bool ready = false;
  std::optional<int> pid;
};

struct HalAdapterFunctionInfo {
  std::string name;
  int owner_id = 0;
  std::string owner_name;
  bool uses_fp = false;
  bool reentrant = false;
  int users = 0;
  std::optional<std::int32_t> runtime;
  std::int32_t max_runtime = 0;
  bool max_runtime_increased = false;
};

struct HalAdapterThreadInfo {
  std::string name;
  std::int64_t period_ns = 0;
  int priority = 0;
  bool uses_fp = false;
  bool running = false;
  std::optional<std::int32_t> runtime;
  std::int32_t max_runtime = 0;
  std::vector<std::string> functions;
};

struct HalAdapterPinInfo {
  std::string name;
  HalAdapterValue value = false;
  HalAdapterType type = HalAdapterType::Bit;
  HalAdapterPinDirection direction = HalAdapterPinDirection::In;
  int owner_id = 0;
  std::optional<std::string> signal_name;
};

struct HalAdapterParamInfo {
  std::string name;
  HalAdapterValue value = false;
  HalAdapterType type = HalAdapterType::Bit;
  HalAdapterParamDirection direction = HalAdapterParamDirection::ReadOnly;
  int owner_id = 0;
};

struct HalAdapterSignalInfo {
  std::string name;
  HalAdapterValue value = false;
  HalAdapterType type = HalAdapterType::Bit;
  std::optional<std::string> driver;
  int readers = 0;
  int writers = 0;
  int bidirs = 0;
};

struct HalAdapterTopology {
  std::vector<HalAdapterComponentInfo> components;
  std::vector<HalAdapterFunctionInfo> functions;
  std::vector<HalAdapterThreadInfo> threads;
  std::vector<HalAdapterPinInfo> pins;
  std::vector<HalAdapterParamInfo> params;
  std::vector<HalAdapterSignalInfo> signals;
};

class HalAdapterError final : public std::runtime_error {
 public:
  explicit HalAdapterError(std::string message, int code = 0);
  int code() const noexcept { return code_; }

 private:
  int code_;
};

/**
 * Owns one server-retained remote HAL proxy. Its lifetime is independent of
 * any individual RPC stream; hal_exit() runs only when the proxy is destroyed
 * or the daemon shuts down.
 */
class LinuxCncHalComponent final {
 public:
  ~LinuxCncHalComponent();
  LinuxCncHalComponent(const LinuxCncHalComponent&) = delete;
  LinuxCncHalComponent& operator=(const LinuxCncHalComponent&) = delete;
  LinuxCncHalComponent(LinuxCncHalComponent&&) noexcept;
  LinuxCncHalComponent& operator=(LinuxCncHalComponent&&) noexcept;

  int id() const noexcept;
  const std::string& name() const noexcept;
  const std::string& prefix() const noexcept;
  bool ready() const noexcept;

  bool add_pin(const std::string& suffix, HalAdapterType type,
               HalAdapterPinDirection direction);
  bool add_param(const std::string& suffix, HalAdapterType type,
                 HalAdapterParamDirection direction);
  void set_ready();
  void set_unready();
  std::optional<HalAdapterValue> read(const std::string& suffix) const;
  bool write(const std::string& suffix, HalAdapterValue value);

 private:
  struct Impl;
  explicit LinuxCncHalComponent(std::unique_ptr<Impl> impl);
  friend class LinuxCncHalAdapter;
  std::unique_ptr<Impl> impl_;
};

/**
 * Transport-neutral LinuxCNC HAL adapter. It owns a small ready observer
 * component to attach to HAL and serializes shared-memory list/value access
 * under HAL's mutex. It intentionally exposes no signal link/unlink methods.
 */
class LinuxCncHalAdapter final {
 public:
  static constexpr std::size_t kDefaultMaxDynamicItems = 4096;

  explicit LinuxCncHalAdapter(
      std::string component_name = "linuxcnc-grpc",
      std::size_t dynamic_item_capacity = kDefaultMaxDynamicItems);
  ~LinuxCncHalAdapter();
  LinuxCncHalAdapter(const LinuxCncHalAdapter&) = delete;
  LinuxCncHalAdapter& operator=(const LinuxCncHalAdapter&) = delete;
  LinuxCncHalAdapter(LinuxCncHalAdapter&&) noexcept;
  LinuxCncHalAdapter& operator=(LinuxCncHalAdapter&&) noexcept;

  int component_id() const noexcept;
  HalAdapterTopology topology() const;
  std::optional<HalAdapterValue> read(
      const HalAdapterReference& reference) const;
  std::vector<std::optional<HalAdapterValue>> read_many(
      const std::vector<HalAdapterReference>& references,
      const std::stop_token& stop_token = {}) const;
  bool write(const HalAdapterReference& reference, HalAdapterValue value,
             HalAdapterValue* written = nullptr);
  std::size_t write_many(
      const std::vector<std::pair<HalAdapterReference, HalAdapterValue>>&
          updates,
      std::vector<HalAdapterValue>* written = nullptr,
      const std::stop_token& stop_token = {});

  bool create_signal(const std::string& name, HalAdapterType type);
  bool pin_has_writer(const std::string& name) const;
  bool component_exists(const std::string& name) const;
  bool component_ready(const std::string& name) const;
  int message_level() const;
  void set_message_level(int level);

  std::unique_ptr<LinuxCncHalComponent> open_component(
      const std::string& name, const std::string& prefix = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace linuxcnc::server
