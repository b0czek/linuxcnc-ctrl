#pragma once

// C++ wrappers around LinuxCNC's public HAL query API (hal_list_*, hal_get_*,
// hal_set_*), shared by the HAL adapter and the scope controller.
//
// Every query function holds HAL's mutex while it runs its callback. That
// mutex is recursive per OS thread, so queries issued from inside a callback
// (or inside with_lock()) see the same locked HAL state. Callbacks run inside
// C code: an exception must not unwind through it, because that would leave
// HAL locked for every process. The wrappers catch any exception thrown by a
// callable, stop the iteration and rethrow it after HAL has released the
// mutex.
//
// Strings in hal_query_t (name, comp, signal, ...) point into HAL shared
// memory and stay valid only while the mutex is held. Copy them inside the
// callable or inside with_lock().

#ifndef ULAPI
#error "hal/query.hpp requires ULAPI"
#endif

#include <hal.h>

#include <cerrno>
#include <exception>
#include <functional>
#include <type_traits>

namespace linuxcnc::server::hal_query {

// Returned by a callable to stop an iteration early without an error.
inline constexpr int kStop = 1;

namespace detail {

template <typename Fn>
struct Context {
  Fn* fn = nullptr;
  std::exception_ptr error;
};

template <typename Fn>
int trampoline(hal_query_t* query, void* arg) noexcept {
  auto* context = static_cast<Context<Fn>*>(arg);
  try {
    if constexpr (std::is_void_v<std::invoke_result_t<Fn&, hal_query_t&>>) {
      std::invoke(*context->fn, *query);
      return 0;
    } else {
      return std::invoke(*context->fn, *query);
    }
  } catch (...) {
    context->error = std::current_exception();
    return kStop;
  }
}

template <typename Api, typename Fn>
int invoke(Api api, hal_query_t* query, Fn& fn) {
  Context<Fn> context{&fn, nullptr};
  const int result = api(query, &trampoline<Fn>, &context);
  if (context.error) std::rethrow_exception(context.error);
  return result;
}

}  // namespace detail

// A callable takes `hal_query_t&` and returns void (continue) or int: zero to
// continue, kStop (positive) to stop, a negative errno to stop with an error.
// Each wrapper returns the HAL function's result: zero after a complete
// iteration, the callable's non-zero value, or a negative HAL error such as
// -EFAULT when HAL is not mapped.

// Pins and/or parameters; `filter` is HAL_QTYPE_ANY, HAL_QTYPE_PIN or
// HAL_QTYPE_PARAM.
template <typename Fn>
int list_pins_params(hal_qtype_t filter, Fn&& fn) {
  hal_query_t query{};
  query.qtype = filter;
  return detail::invoke(&hal_list_p, &query, fn);
}

// Pins linked to the named signal, in HAL's list order. -ENOENT if the
// signal does not exist.
template <typename Fn>
int list_signal_pins(const char* signal, Fn&& fn) {
  hal_query_t query{};
  query.name = signal;
  return detail::invoke(&hal_list_p_s, &query, fn);
}

template <typename Fn>
int list_signals(Fn&& fn) {
  hal_query_t query{};
  return detail::invoke(&hal_list_s, &query, fn);
}

template <typename Fn>
int list_components(Fn&& fn) {
  hal_query_t query{};
  return detail::invoke(&hal_list_comp, &query, fn);
}

template <typename Fn>
int list_functions(Fn&& fn) {
  hal_query_t query{};
  return detail::invoke(&hal_list_funct, &query, fn);
}

// Threads; with `with_functions` the callable is also invoked once per
// attached function (query.qtype == HAL_QTYPE_THREAD_FUNCT) after its thread.
template <typename Fn>
int list_threads(bool with_functions, Fn&& fn) {
  hal_query_t query{};
  query.qtype = with_functions ? HAL_QTYPE_THREAD_FUNCT : HAL_QTYPE_THREAD;
  return detail::invoke(&hal_list_thread, &query, fn);
}

// Runs `fn()` once while holding HAL's mutex, so that several queries made
// inside it observe one consistent HAL state. Exceptions from `fn` are
// rethrown after the mutex is released.
//
// The public API has no plain lock call; this borrows the mutex of
// hal_list_comp() and stops at the first component. It therefore requires at
// least one HAL component to exist, which holds for any caller that has
// called hal_init(). Returns 0 on success, -ENODEV when no component exists,
// or a negative HAL error.
template <typename Fn>
int with_lock(Fn&& fn) {
  bool ran = false;
  auto once = [&](hal_query_t&) {
    ran = true;
    std::invoke(fn);
    return kStop;
  };
  const int result = list_components(once);
  if (result < 0) return result;
  return ran ? 0 : -ENODEV;
}

}  // namespace linuxcnc::server::hal_query
