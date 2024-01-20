/* Flow-IPC: Sessions
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

/// @file
#pragma once

#include "ipc/session/sync_io/detail/session_adapter.hpp"

namespace ipc::session::sync_io
{

// Types.

/**
 * `sync_io`-pattern counterpart to async-I/O-pattern session::Server_session types and all their SHM-aware variations
 * (at least shm::classic::Server_session et al and shm::arena_lend::jemalloc::Server_session et al).  In point of fact:
 *   - Use this if and only if you desire a `sync_io`-pattern style of being informed of async events from a
 *     `Server_session` of any kind.  For example, you may find this convenient if your event loop is an old-school
 *     reactor using `poll()` or `epoll_wait()`.  This affects exactly the following APIs:
 *     - `Server_session` reporting a session-hosing error via on-error handler.
 *       - Set up via init_handlers().
 *     - `Server_session` reporting a channel having been passively-opened via that handler.
 *       - Set up via init_handlers().
 *   - This Server_session_adapter *adapts* a `Server_session` (earlier filled-out by
 *     Session_server_adapter::async_accept()) stored within `*this`.  All APIs excluding the above -- that is to
 *     say all non-async APIs -- are to be invoked via core() accessor.
 *     - Trying to use `core()->init_handlers()` leads to undefined behavior.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 * @see Session_server_adapter which prepares useful objects of this type.
 * @see session::Server_session_mv, session::Server_session, et al.
 *
 * As is generally the case when choosing `sync_io::X` versus `X`, we recommend using `X` due to it being easier.
 * In this particular case (see below) there is no perf benefit to using `sync_io::X`, either, so the only reason
 * to use `sync_io::X` in this case would be because you've got an old-school reactor event loop with
 * a `poll()` or `epoll_wait()`, in which case the `sync_io` API may be easier to integrate.
 *
 * To use it:
 *   - Determine the type of your desired Session_server_adapter.  For example, if you deal with
 *     sessions of type `session::shm::classic::Session<knobs>`, then use
 *     `S = Session_server_adapter<session::shm::classic::Session_server<knobs>>`.
 *   - From this obtain the desired type of `*this`: `using T = S::Session_obj`.
 *   - Construct a blank `T t` (via default ctor) -- a/k/a `*this`.
 *   - Use `S::async_accept(&t)`, targeting `*this`.  On success `*this` is ready to use, in almost-PEER state.
 *   - Set up `sync_io` pattern using start_ops() (and if needed precede it with replace_event_wait_handles()).
 *   - Call init_handlers(), analogously to async-I/O Server_session_mv::init_handlers().  As with an async-I/O
 *     `Server_session` you will need to provide:
 *     - Error handler (though it will be invoked synchronously per `sync_io` pattern).
 *     - (Optional -- if you want to enable channel passive-open): Channel passive-open handler
 *       (though it will be invoked via... ditto).
 *   - Be ready for error handler to fire (in `sync_io` style).
 *   - Be ready for passive-channel-open handler to fire (in `sync_io` style).
 *   - Use `core()->` for all other API needs (e.g., `open_channel()`, `session_shm()` -- if SHM-aware).
 *
 * ### Internal implementation ###
 * Normally this would not be in the public docs for this public-use class, but indulge us...
 *
 * ...by reading an equally applicable note in Client_session_adapter doc header.
 *
 * @tparam Session
 *         The async-I/O `Server_session` concrete type being adapted.  As of this writing that would be one of
 *         at least: `session::Server_session<knobs>`, `session::shm::classic::Server_session<knobs>`,
 *         `session::shm::jemalloc::Server_session<knobs>`.  We would recommend the technique shown in the
 *         above doc header involving Session_server_adapter::Session_obj.  This will enable nice code reuse and
 *         be conducive to generic programming.
 */
template<typename Session>
class Server_session_adapter :
  private Session_adapter<Session>,
  private boost::noncopyable // There's a to-do to make it movable.
{
private:
  // Types.

  /// Our main base.
  using Base = Session_adapter<Session>;

public:
  // Types.

  /// Short-hand, for generic programming et al, for template parameter `Session`.
  using Session_obj = typename Base::Session_obj;

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = typename Base::Async_io_obj;
  /// You may disregard.
  using Sync_io_obj = typename Base::Sync_io_obj;

  // Constructors/destructor.

  /// Forwards to the #Session_obj default ctor.
  Server_session_adapter();

  // Methods.

  /**
   * All notes from Client_session_adapter::start_ops() apply equally.
   *
   * @tparam Event_wait_func_t
   *         See above.
   * @param ev_wait_func
   *        See above.
   * @return See above.
   */
  template<typename Event_wait_func_t>
  bool start_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * All notes from Client_session_adapter::replace_event_wait_handles() apply equally.
   *
   * @tparam Create_ev_wait_hndl_func
   *         See above.
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Acts identically to all overloads of Server_session_mv::init_handlers(), except that the completion handler(s)
   * are invoked in the `sync_io`-pattern fashion, synchronously inside an async-wait performed by you and
   * reported via `(*on_active_ev_func)()`.  Returns `false` if invoked before start_ops() in addition to the
   * possible reasons per Server_session_mv::init_handlers().
   *
   * @tparam Args
   *         See above.
   * @param args
   *        See above.
   * @return See above.
   */
  template<typename... Args>
  bool init_handlers(Args&&... args);

  /**
   * The adapted mutable #Session_obj.  It is safe to access any API except for `core()->init_handlers()` (undefined
   * behavior); use `this->init_handlers()` instead.  Remember that start_ops() is required first.
   *
   * @return See above.
   */
  Session_obj* core();

  /**
   * The adapted immutable #Session_obj.  Remember that start_ops() is required first.
   *
   * @return See above.
   */
  const Session_obj* core() const;

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using Base::get_logger;
  using Base::get_log_component;
}; // class Server_session_adapter

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Session>
Server_session_adapter<Session>::Server_session_adapter() = default;

template<typename Session>
template<typename Event_wait_func_t>
bool Server_session_adapter<Session>::start_ops(Event_wait_func_t&& ev_wait_func)
{
  return Base::start_ops(std::move(ev_wait_func));
} // Session_adapter::start_ops()

template<typename Session>
template<typename Create_ev_wait_hndl_func>
bool Server_session_adapter<Session>::replace_event_wait_handles
       (const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  return Base::replace_event_wait_handles(create_ev_wait_hndl_func);
}

template<typename Session>
template<typename... Args>
bool Server_session_adapter<Session>::init_handlers(Args&&... args)
{
  return Base::init_handlers(std::forward<Args>(args)...);
}

template<typename Session>
typename Server_session_adapter<Session>::Session_obj*
  Server_session_adapter<Session>::core()
{
  return Base::core();
}

template<typename Session>
const typename Server_session_adapter<Session>::Session_obj*
  Server_session_adapter<Session>::core() const
{
  return const_cast<Server_session_adapter*>(this)->core();
}

template<typename Session>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_adapter<Session>& val)
{
  return os << "SIO@" << static_cast<const void*>(&val) << " srv_sess[" << (*(val.core())) << ']';
}

} // namespace ipc::session::sync_io
