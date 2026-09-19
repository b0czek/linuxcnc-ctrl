#include "hal/grpc/remote_component_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "linuxcnc_grpc/hal/grpc/mapping.hpp"

namespace linuxcnc::server::detail {
namespace {

using namespace linuxcnc::v1;

constexpr char kOnlinePin[] = "online";

::grpc::Status invalid(const std::string& message) {
  return {::grpc::StatusCode::INVALID_ARGUMENT, message};
}

::grpc::Status hal_error(const HalAdapterError& error) {
  const auto code =
      error.code() == -ENOENT   ? ::grpc::StatusCode::NOT_FOUND
      : error.code() == -EBUSY  ? ::grpc::StatusCode::RESOURCE_EXHAUSTED
      : error.code() == -EINVAL ? ::grpc::StatusCode::INVALID_ARGUMENT
                                : ::grpc::StatusCode::FAILED_PRECONDITION;
  return {code, error.what()};
}

std::optional<HalAdapterType> decode_hal_type(HalType type) {
  if (type < HAL_TYPE_BIT || type > HAL_TYPE_U64) return std::nullopt;
  return static_cast<HalAdapterType>(static_cast<int>(type) - 1);
}

std::string random_bytes(std::size_t size) {
  std::random_device source;
  std::string value(size, '\0');
  for (auto& byte : value) byte = static_cast<char>(source());
  return value;
}

std::string random_proxy_id() {
  const auto bytes = random_bytes(16);
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const unsigned char byte : bytes) stream << std::setw(2) << +byte;
  return stream.str();
}

bool token_equal(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index)
    difference |= static_cast<unsigned char>(left[index] ^ right[index]);
  return difference == 0;
}

bool typed_value(HalAdapterType type, const HalScalar& scalar,
                 HalAdapterValue* value) {
  auto decoded = decode_hal_scalar(scalar);
  if (!decoded || decoded->index() != static_cast<std::size_t>(type))
    return false;
  if (value) *value = *decoded;
  return true;
}

}  // namespace

struct RemoteComponentRegistry::Impl {
  struct Proxy {
    struct Item {
      std::string suffix;
      HalItemKind kind = HAL_ITEM_KIND_PIN;
      HalAdapterType type = HalAdapterType::Bit;
      std::string full_name;
      bool client_writable = false;
      bool server_readable = false;
      std::optional<HalAdapterValue> disconnect_value;
      std::optional<HalAdapterValue> previous;
    };

    std::string id;
    std::string ownership_token;
    std::unique_ptr<LinuxCncHalComponent> component;
    std::vector<Item> items;
    std::chrono::milliseconds sampling_period{20};
    std::chrono::steady_clock::time_point next_sample;
    std::chrono::steady_clock::time_point last_heartbeat;
    std::uint64_t generation = 0;
    std::uint64_t client_sequence = 0;
    std::uint64_t server_sequence = 0;
    bool attached = false;
    bool active = false;
    RemoteComponentCallbacks callbacks;
  };

  LinuxCncHalAdapter& adapter;
  AdmissionCounter& component_admission;
  const std::size_t max_items;
  const std::chrono::milliseconds heartbeat_interval;
  const std::chrono::milliseconds heartbeat_timeout;
  const std::chrono::milliseconds default_sampling_period;
  const std::chrono::milliseconds min_sampling_period;
  const std::chrono::milliseconds max_sampling_period;
  std::unordered_map<std::string, std::unique_ptr<Proxy>> proxies;
  std::size_t retained_item_count = 0;

  Impl(LinuxCncHalAdapter& adapter, AdmissionCounter& component_admission,
       const DaemonConfig& config)
      : adapter(adapter),
        component_admission(component_admission),
        max_items(config.max_remote_hal_items),
        heartbeat_interval(config.component_heartbeat_interval),
        heartbeat_timeout(config.component_heartbeat_timeout),
        default_sampling_period(config.component_default_sampling_period),
        min_sampling_period(config.component_min_sampling_period),
        max_sampling_period(config.component_max_sampling_period) {}

  static Proxy::Item* find_item(Proxy& proxy, const HalItemRef& ref) {
    auto name = ref.name();
    const auto prefix = proxy.component->prefix() + ".";
    if (name.rfind(prefix, 0) == 0) name.erase(0, prefix.size());
    const auto found = std::find_if(
        proxy.items.begin(), proxy.items.end(), [&](const auto& item) {
          return item.suffix == name && item.kind == ref.kind();
        });
    return found == proxy.items.end() ? nullptr : &*found;
  }

  static ::grpc::Status decode_values(
      Proxy& proxy,
      const google::protobuf::RepeatedPtrField<ComponentValue>& values,
      bool complete,
      std::vector<std::pair<Proxy::Item*, HalAdapterValue>>* decoded) {
    std::unordered_set<Proxy::Item*> seen;
    decoded->clear();
    decoded->reserve(values.size());
    for (const auto& update : values) {
      auto* item = find_item(proxy, update.item());
      if (!item || !item->client_writable)
        return invalid(
            "component update targets an item not owned by the client");
      if (!seen.insert(item).second)
        return invalid("component update contains a duplicate item");
      HalAdapterValue value;
      if (!typed_value(item->type, update.value(), &value))
        return invalid("component update value has the wrong exact type");
      decoded->emplace_back(item, value);
    }
    if (complete) {
      const auto expected = static_cast<std::size_t>(
          std::count_if(proxy.items.begin(), proxy.items.end(),
                        [](const auto& item) { return item.client_writable; }));
      if (seen.size() != expected)
        return invalid(
            "activation requires complete client-owned output state");
    }
    return ::grpc::Status::OK;
  }

  void encode_snapshot(Proxy& proxy, HalComponentAttached* attached,
                       bool include_token) const {
    attached->set_proxy_id(proxy.id);
    attached->set_name(proxy.component->name());
    attached->set_prefix(proxy.component->prefix());
    attached->set_generation(proxy.generation);
    attached->set_sampling_period_ms(proxy.sampling_period.count());
    attached->set_heartbeat_interval_ms(heartbeat_interval.count());
    attached->set_heartbeat_timeout_ms(heartbeat_timeout.count());
    if (include_token) attached->set_ownership_token(proxy.ownership_token);
    for (auto& item : proxy.items) {
      const auto value = proxy.component->read(item.suffix);
      if (!value) continue;
      item.previous = value;
      auto* encoded = attached->add_retained_values();
      encoded->mutable_item()->set_kind(item.kind);
      encoded->mutable_item()->set_name(item.full_name);
      encode_hal_scalar(*value, encoded->mutable_value());
    }
  }

  static void apply_disconnect(Proxy& proxy) {
    proxy.component->write(kOnlinePin, HalAdapterValue{false});
    proxy.active = false;
    for (auto& item : proxy.items)
      if (item.disconnect_value)
        proxy.component->write(item.suffix, *item.disconnect_value);
  }

  void detach(const std::string& id, std::uint64_t generation) {
    const auto found = proxies.find(id);
    if (found == proxies.end()) return;
    auto& proxy = *found->second;
    if (!proxy.attached || proxy.generation != generation) return;
    apply_disconnect(proxy);
    proxy.attached = false;
    proxy.callbacks = {};
  }

  RemoteComponentResult create(const HalComponentCreate& create,
                               RemoteComponentCallbacks callbacks) {
    RemoteComponentResult result;
    const std::string prefix =
        create.prefix().empty() ? create.name() : create.prefix();
    if (create.name().empty() || prefix.empty()) {
      result.status = invalid("component name and prefix are required");
      return result;
    }
    if (std::any_of(proxies.begin(), proxies.end(), [&](const auto& entry) {
          return entry.second->component->name() == create.name();
        })) {
      result.status = {::grpc::StatusCode::ALREADY_EXISTS,
                       "remote component name already exists"};
      return result;
    }

    std::unordered_set<std::string> names{kOnlinePin};
    struct Definition {
      std::string name;
      HalItemKind kind;
      HalAdapterType type;
      int direction;
      std::optional<HalAdapterValue> disconnect;
    };
    std::vector<Definition> definitions;
    definitions.reserve(create.pins_size() + create.parameters_size());
    for (const auto& pin : create.pins()) {
      const auto type = decode_hal_type(pin.type());
      if (!type || pin.name().empty() || !names.insert(pin.name()).second ||
          pin.direction() < HAL_PIN_DIRECTION_IN ||
          pin.direction() > HAL_PIN_DIRECTION_IO) {
        result.status = invalid(
            "component pin schema is invalid or uses a reserved/duplicate "
            "name");
        return result;
      }
      std::optional<HalAdapterValue> disconnect;
      if (pin.has_disconnect_value()) {
        HalAdapterValue value;
        if ((pin.direction() != HAL_PIN_DIRECTION_OUT &&
             pin.direction() != HAL_PIN_DIRECTION_IO) ||
            !typed_value(*type, pin.disconnect_value(), &value)) {
          result.status = invalid(
              "component pin disconnect value is not allowed or has the "
              "wrong type");
          return result;
        }
        disconnect = value;
      }
      definitions.push_back({pin.name(), HAL_ITEM_KIND_PIN, *type,
                             static_cast<int>(pin.direction()), disconnect});
    }
    for (const auto& parameter : create.parameters()) {
      const auto type = decode_hal_type(parameter.type());
      if (!type || parameter.name().empty() ||
          !names.insert(parameter.name()).second ||
          parameter.direction() < HAL_PARAM_DIRECTION_RO ||
          parameter.direction() > HAL_PARAM_DIRECTION_RW) {
        result.status = invalid(
            "component parameter schema is invalid or uses a "
            "reserved/duplicate name");
        return result;
      }
      std::optional<HalAdapterValue> disconnect;
      if (parameter.has_disconnect_value()) {
        HalAdapterValue value;
        if (!typed_value(*type, parameter.disconnect_value(), &value)) {
          result.status = invalid(
              "component parameter disconnect value has the wrong type");
          return result;
        }
        disconnect = value;
      }
      definitions.push_back({parameter.name(), HAL_ITEM_KIND_PARAM, *type,
                             static_cast<int>(parameter.direction()),
                             disconnect});
    }

    const std::size_t item_count = definitions.size() + 1;
    if (item_count > max_items ||
        retained_item_count > max_items - item_count ||
        !component_admission.acquire()) {
      result.status = {::grpc::StatusCode::RESOURCE_EXHAUSTED,
                       "remote HAL component or item budget exhausted"};
      return result;
    }
    retained_item_count += item_count;
    try {
      auto proxy = std::make_unique<Proxy>();
      proxy->id = random_proxy_id();
      while (proxies.contains(proxy->id)) proxy->id = random_proxy_id();
      proxy->ownership_token = random_bytes(32);
      proxy->component = adapter.open_component(create.name(), prefix);
      proxy->items.reserve(item_count);
      for (const auto& definition : definitions) {
        bool added = false;
        bool writable = false;
        bool readable = false;
        if (definition.kind == HAL_ITEM_KIND_PIN) {
          const auto direction =
              static_cast<HalPinDirection>(definition.direction);
          const auto adapter_direction =
              direction == HAL_PIN_DIRECTION_IN    ? HalAdapterPinDirection::In
              : direction == HAL_PIN_DIRECTION_OUT ? HalAdapterPinDirection::Out
                                                   : HalAdapterPinDirection::Io;
          added = proxy->component->add_pin(definition.name, definition.type,
                                            adapter_direction);
          writable = direction != HAL_PIN_DIRECTION_IN;
          readable = direction != HAL_PIN_DIRECTION_OUT;
        } else {
          const auto direction =
              static_cast<HalParamDirection>(definition.direction);
          added = proxy->component->add_param(
              definition.name, definition.type,
              direction == HAL_PARAM_DIRECTION_RW
                  ? HalAdapterParamDirection::ReadWrite
                  : HalAdapterParamDirection::ReadOnly);
          writable = true;
          readable = direction == HAL_PARAM_DIRECTION_RW;
        }
        if (!added)
          throw HalAdapterError("component item was rejected", -EINVAL);
        proxy->items.push_back({definition.name, definition.kind,
                                definition.type, prefix + "." + definition.name,
                                writable, readable, definition.disconnect,
                                std::nullopt});
      }
      if (!proxy->component->add_pin(kOnlinePin, HalAdapterType::Bit,
                                     HalAdapterPinDirection::Out))
        throw HalAdapterError("managed online pin was rejected", -EINVAL);
      proxy->items.push_back({kOnlinePin, HAL_ITEM_KIND_PIN,
                              HalAdapterType::Bit, prefix + ".online", false,
                              false, std::nullopt, std::nullopt});
      proxy->component->write(kOnlinePin, HalAdapterValue{false});
      proxy->component->set_ready();
      const auto requested =
          create.sampling_period_ms() == 0
              ? default_sampling_period
              : std::chrono::milliseconds(create.sampling_period_ms());
      proxy->sampling_period =
          std::clamp(requested, min_sampling_period, max_sampling_period);
      proxy->next_sample = std::chrono::steady_clock::now();
      proxy->last_heartbeat = std::chrono::steady_clock::now();
      proxy->generation = 1;
      proxy->attached = true;
      proxy->callbacks = std::move(callbacks);
      HalComponentServerMessage message;
      encode_snapshot(*proxy, message.mutable_attached(), true);
      result.proxy_id = proxy->id;
      result.generation = proxy->generation;
      result.response = std::move(message);
      const auto proxy_id = proxy->id;
      proxies.emplace(proxy_id, std::move(proxy));
      return result;
    } catch (const HalAdapterError& error) {
      retained_item_count -= item_count;
      component_admission.release();
      result.status = hal_error(error);
      return result;
    } catch (const std::exception& error) {
      retained_item_count -= item_count;
      component_admission.release();
      result.status = {
          ::grpc::StatusCode::INTERNAL,
          std::string("remote component creation failed: ") + error.what()};
      return result;
    } catch (...) {
      retained_item_count -= item_count;
      component_admission.release();
      result.status = {::grpc::StatusCode::INTERNAL,
                       "remote component creation failed"};
      return result;
    }
  }

  RemoteComponentResult attach(const HalComponentAttach& attach,
                               RemoteComponentCallbacks callbacks) {
    RemoteComponentResult result;
    const auto found = proxies.find(attach.proxy_id());
    if (found == proxies.end() ||
        found->second->component->name() != attach.name()) {
      result.status = {::grpc::StatusCode::NOT_FOUND,
                       "remote component proxy was not found"};
      return result;
    }
    auto& proxy = *found->second;
    if (!token_equal(proxy.ownership_token, attach.ownership_token())) {
      result.status = {::grpc::StatusCode::UNAUTHENTICATED,
                       "invalid remote component ownership token"};
      return result;
    }
    if (proxy.attached) {
      result.status = {::grpc::StatusCode::ALREADY_EXISTS,
                       "remote component already has an active attachment"};
      return result;
    }
    proxy.attached = true;
    proxy.active = false;
    ++proxy.generation;
    proxy.client_sequence = 0;
    proxy.last_heartbeat = std::chrono::steady_clock::now();
    proxy.callbacks = std::move(callbacks);
    proxy.component->write(kOnlinePin, HalAdapterValue{false});
    HalComponentServerMessage message;
    encode_snapshot(proxy, message.mutable_attached(), false);
    result.proxy_id = proxy.id;
    result.generation = proxy.generation;
    result.response = std::move(message);
    return result;
  }

  RemoteComponentResult consume(const std::string& attached_id,
                                std::uint64_t attached_generation,
                                RemoteComponentCallbacks callbacks,
                                const HalComponentClientMessage& request) {
    RemoteComponentResult result;
    const bool first = attached_id.empty();
    if (first && !request.has_create() && !request.has_attach()) {
      result.status =
          invalid("first component message must be Create or Attach");
      return result;
    }
    if (!first && (request.has_create() || request.has_attach())) {
      result.status = invalid("component stream is already attached");
      return result;
    }
    if (request.has_create())
      return create(request.create(), std::move(callbacks));
    if (request.has_attach())
      return attach(request.attach(), std::move(callbacks));

    const auto found = proxies.find(attached_id);
    if (found == proxies.end() || !found->second->attached ||
        found->second->generation != attached_generation) {
      result.status = {::grpc::StatusCode::FAILED_PRECONDITION,
                       "component attachment generation is stale"};
      return result;
    }
    auto& proxy = *found->second;
    result.proxy_id = proxy.id;
    result.generation = proxy.generation;
    std::vector<std::pair<Proxy::Item*, HalAdapterValue>> values;
    if (request.has_activate()) {
      if (proxy.active || request.activate().generation() != proxy.generation) {
        result.status = {
            ::grpc::StatusCode::FAILED_PRECONDITION,
            "component is already active or its generation is stale"};
        return result;
      }
      result.status =
          decode_values(proxy, request.activate().values(), true, &values);
      if (!result.status.ok()) return result;
      for (const auto& [item, value] : values)
        if (!proxy.component->write(item->suffix, value)) {
          result.status = invalid("component activation could not be applied");
          return result;
        }
      proxy.component->write(kOnlinePin, HalAdapterValue{true});
      proxy.active = true;
      proxy.last_heartbeat = std::chrono::steady_clock::now();
      HalComponentServerMessage message;
      message.mutable_active()->set_generation(proxy.generation);
      result.response = std::move(message);
      return result;
    }
    if (request.has_update()) {
      if (!proxy.active || request.update().generation() != proxy.generation) {
        result.status = {::grpc::StatusCode::FAILED_PRECONDITION,
                         "component update is inactive or stale"};
        return result;
      }
      if (request.update().sequence() != proxy.client_sequence + 1) {
        result.status = {::grpc::StatusCode::OUT_OF_RANGE,
                         "component update sequence is not contiguous"};
        return result;
      }
      result.status =
          decode_values(proxy, request.update().values(), false, &values);
      if (!result.status.ok()) return result;
      for (const auto& [item, value] : values)
        if (!proxy.component->write(item->suffix, value)) {
          result.status = invalid("component update could not be applied");
          return result;
        }
      proxy.client_sequence = request.update().sequence();
      proxy.last_heartbeat = std::chrono::steady_clock::now();
      HalComponentServerMessage message;
      auto* ack = message.mutable_update_ack();
      ack->set_generation(proxy.generation);
      ack->set_sequence(proxy.client_sequence);
      result.response = std::move(message);
      return result;
    }
    if (request.has_heartbeat()) {
      if (request.heartbeat().generation() != proxy.generation) {
        result.status = {::grpc::StatusCode::FAILED_PRECONDITION,
                         "component heartbeat generation is stale"};
        return result;
      }
      proxy.last_heartbeat = std::chrono::steady_clock::now();
      HalComponentServerMessage message;
      message.mutable_heartbeat_ack()->set_generation(proxy.generation);
      result.response = std::move(message);
      return result;
    }
    if (request.has_close()) {
      if (request.close().generation() != proxy.generation ||
          (request.close().mode() != HAL_COMPONENT_CLOSE_MODE_DETACH &&
           request.close().mode() != HAL_COMPONENT_CLOSE_MODE_DESTROY)) {
        result.status =
            invalid("component close mode or generation is invalid");
        return result;
      }
      const auto mode = request.close().mode();
      apply_disconnect(proxy);
      proxy.attached = false;
      proxy.callbacks = {};
      HalComponentServerMessage message;
      message.mutable_closed()->set_proxy_id(proxy.id);
      message.mutable_closed()->set_mode(mode);
      if (mode == HAL_COMPONENT_CLOSE_MODE_DESTROY) {
        retained_item_count -= proxy.items.size();
        proxies.erase(found);
        component_admission.release();
      }
      result.response = std::move(message);
      result.close = true;
      return result;
    }
    result.status = invalid("client sent an invalid component message");
    return result;
  }

  void sample(std::chrono::steady_clock::time_point now) {
    for (auto& [id, owned] : proxies) {
      (void)id;
      auto& proxy = *owned;
      if (!proxy.attached) continue;
      if (now - proxy.last_heartbeat >= heartbeat_timeout) {
        auto timeout = proxy.callbacks.heartbeat_timeout;
        const auto generation = proxy.generation;
        apply_disconnect(proxy);
        proxy.attached = false;
        proxy.callbacks = {};
        if (timeout) timeout(generation);
        continue;
      }
      if (!proxy.active || now < proxy.next_sample) continue;
      proxy.next_sample = now + proxy.sampling_period;
      HalComponentServerMessage message;
      auto* delta = message.mutable_delta();
      delta->set_generation(proxy.generation);
      for (auto& item : proxy.items) {
        if (!item.server_readable) continue;
        const auto value = proxy.component->read(item.suffix);
        if (!value || (item.previous && *item.previous == *value)) continue;
        item.previous = value;
        auto* encoded = delta->add_values();
        encoded->mutable_item()->set_kind(item.kind);
        encoded->mutable_item()->set_name(item.full_name);
        encode_hal_scalar(*value, encoded->mutable_value());
      }
      if (delta->values_size() == 0) continue;
      delta->set_sequence(++proxy.server_sequence);
      if (proxy.callbacks.offer_delta)
        proxy.callbacks.offer_delta(std::move(message));
    }
  }

  void shutdown() {
    const auto count = proxies.size();
    proxies.clear();
    retained_item_count = 0;
    for (std::size_t index = 0; index < count; ++index)
      component_admission.release();
  }
};

RemoteComponentRegistry::RemoteComponentRegistry(
    LinuxCncHalAdapter& adapter, AdmissionCounter& component_admission,
    const DaemonConfig& config)
    : impl_(std::make_unique<Impl>(adapter, component_admission, config)) {}

RemoteComponentRegistry::~RemoteComponentRegistry() {
  if (impl_) impl_->shutdown();
}

RemoteComponentResult RemoteComponentRegistry::consume(
    const std::string& attached_id, std::uint64_t attached_generation,
    RemoteComponentCallbacks callbacks,
    const HalComponentClientMessage& request) {
  return impl_->consume(attached_id, attached_generation, std::move(callbacks),
                        request);
}

void RemoteComponentRegistry::detach(const std::string& proxy_id,
                                     std::uint64_t generation) {
  impl_->detach(proxy_id, generation);
}

void RemoteComponentRegistry::sample(
    std::chrono::steady_clock::time_point now) {
  impl_->sample(now);
}

void RemoteComponentRegistry::shutdown() { impl_->shutdown(); }

}  // namespace linuxcnc::server::detail
