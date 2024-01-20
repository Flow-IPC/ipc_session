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

#include "ipc/session/sync_io/server_session_adapter.hpp"

namespace ipc::session::sync_io
{

// Types.

/**
 * `sync_io`-pattern counterpart to async-I/O-pattern session::Session_server types and all their SHM-aware variations
 * (at least shm::classic::Session_server and shm::arena_lend::jemalloc::Session_server).  In point of fact:
 *   - Use this if and only if you desire a `sync_io`-pattern style of being informed of async events from a
 *     `Session_server` *and* `Server_session` of any kind.  For example, you may find this convenient
 *     if your event loop is an old-school reactor using `poll()` or `epoll_wait()`.  This affects exactly the
 *     following APIs:
 *     - `Session_server::async_accept()`,
 *   - This Session_server_adapter *adapts* a `Session_server` constructed and stored within `*this`.
 *     All APIs excluding the above -- that is to say all non-async APIs -- are to be invoked via core() accessor.
 *     - Trying to use `core()->async_accept()` leads to undefined behavior.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 * @see session::Session_server, shm::classic::Session_server, shm::arena_lend::jemalloc::Session_server.
 *
 * As is generally the case when choosing `sync_io::X` versus `X`, we recommend using `X` due to it being easier.
 * In this particular case (see below) there is no perf benefit to using `sync_io::X`, either, so the only reason
 * to use `sync_io::X` in this case would be because you've got an old-school reactor event loop with
 * a `poll()` or `epoll_wait()`, in which case the `sync_io` API may be easier to integrate.
 *
 * To use it:
 *   - Construct it explicitly.  The ctor signature is exactly identical to that of session::Session_server.
 *   - Set up `sync_io` pattern using start_ops() (and if needed precede it with replace_event_wait_handles()).
 *   - Use async_accept() in similar fashion to async-I/O `Server_session` supplying a completion handler
 *     (though it will be invoked synchronously per `sync_io` pattern).
 *     - As normal, construct a blank `Server_session` to pass as the target of async_accept().
 *       The type of this object shall be Session_server_adapter::Session_obj; it shall be a concrete
 *       type of the class template Session_server_adapter.
 *   - On successful accept:
 *     - See doc header for Session_server_adapter on what to do next.  Spoiler alert:
 *       Server_session_adapter::start_ops(),
 *       then Server_session_adapter::init_handlers() (mirroring a vanilla `Server_session`).
 *     - Use `core()->` for all other API needs (e.g., shm::classic::Session_server::app_shm()).
 *
 * ### Internal implementation ###
 * Normally this would not be in the public docs for this public-use class, but indulge us...
 *
 * ...by reading an equally applicable note in Client_session_adapter doc header.
 *
 * @tparam Session_server
 *         The async-I/O `Session_server` concrete type being adapted.  As of this writing that would be one of
 *         at least: `session::Session_server<knobs>`, `session::shm::classic::Session_server<knobs>`,
 *         `session::shm::jemalloc::Session_server<knobs>`.
 */
template<typename Session_server>
class Session_server_adapter
{
public:
  // Types.

  /// Short-hand, for generic programming et al, for template parameter `Session_server`.
  using Session_server_obj = Session_server;

  /// Short-hand for object type targeted by async_accept().
  using Session_obj = Server_session_adapter<typename Session_server_obj::Server_session_obj>;

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = Session_server_obj;
  /// You may disregard.
  using Sync_io_obj = transport::Null_peer;

  // Constructors/destructor.

  /**
   * Forwards to the #Session_server_obj ctor.  See Session_server ctor doc headers.
   *
   * @tparam Ctor_args
   *         See above.
   * @param ctor_args
   *        See above.
   */
  template<typename... Ctor_args>
  Session_server_adapter(Ctor_args&&... ctor_args);

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
   * Acts identically to 1-arg overload of Session_server::async_accept(), except that the completion handler
   * is invoked in the `sync_io`-pattern fashion, synchronously inside an async-wait performed by you and
   * reported via `(*on_active_ev_func)()`.  Returns `false` if invoked before start_ops().
   *
   * @tparam Task_err
   *         See above.
   * @param target_session
   *        Pointer to #Session_obj which shall be assigned an almost-PEER-state (open, requires
   *        Server_session_adapter::init_handlers() to enter PEER state) as `on_done_func()`
   *        is called.  Not touched on error.  Recommend default-constructing a #Session_obj and passing
   *        pointer thereto here.
   * @param on_done_func
   *        See above.
   * @return `true` on successful start to async-accept; `false` if called before start_ops().
   */
  template<typename Task_err>
  bool async_accept(Session_obj* target_session, Task_err&& on_done_func);

  /**
   * Acts identically to 7-arg overload of Session_server::async_accept(), except that the completion handler
   * is invoked in the `sync_io`-pattern fashion, synchronously inside an async-wait performed by you and
   * reported via `(*on_active_ev_func)()`.  Returns `false` if invoked before start_ops().
   *
   * @tparam Task_err
   *         See above.
   * @tparam N_init_channels_by_srv_req_func
   *         See above.
   * @tparam Mdt_load_func
   *         See above.
   * @param target_session
   *        See above.
   * @param init_channels_by_srv_req
   *        See above.
   * @param mdt_from_cli_or_null
   *        See above: null or pointer to `Reader` of metadata which shall be set for access on success.
   * @param init_channels_by_cli_req
   *        See above.
   * @param n_init_channels_by_srv_req_func
   *        See above.
   * @param mdt_load_func
   *        See above.
   * @param on_done_func
   *        See above.
   * @return `true` on successful start to async-accept; `false` if called before start_ops().
   */
  template<typename Task_err,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  bool async_accept(Session_obj* target_session,
                    typename Session_server_obj::Channels* init_channels_by_srv_req,
                    typename Session_server_obj::Mdt_reader_ptr* mdt_from_cli_or_null,
                    typename Session_server_obj::Channels* init_channels_by_cli_req,
                    N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                    Mdt_load_func&& mdt_load_func,
                    Task_err&& on_done_func);

  /**
   * The adapted mutable #Session_server_obj.  It is safe to access any API except for `core()->async_accept()`
   * (undefined behavior); use `this->async_accept()` instead.  Remember that start_ops() is required first.
   *
   * @return See above.
   */
  Session_server_obj* core();

  /**
   * The adapted immutable #Session_server_obj.  Remember that start_ops() is required first.
   *
   * @return See above.
   */
  const Session_server_obj* core() const;

  /**
   * See `flow::log::Log_context`.
   * @return See above.
   */
  flow::log::Logger* get_logger() const;

  /**
   * See `flow::log::Log_context`.
   * @return See above.
   */
  const flow::log::Component& get_log_component() const;

private:
  // Methods.

  /**
   * The real handler given for `on_done_func` to `Session_server_obj::async_accept()`: it records the
   * result of that async-accept to #m_target_err_code, then signals accept_read() via
   * the IPC-pipe.
   *
   * @param err_code
   *        Result from `Session_server_obj::async_accept()`.
   */
  void accept_write(const Error_code& err_code);

  /**
   * Signaled by accept_write(), it returns the IPC-pipe to steady-state (empty, not readable), then invokes
   * the original user `on_done_func()`.
   */
  void accept_read();

  // Data.

  /// Similar to the one in Session_adapter.
  flow::util::Task_engine m_nb_task_engine;

  /// Similar to the one in Session_adapter.
  flow::util::Task_engine m_ev_hndl_task_engine_unused;

  /// Similar to the one in Session_adapter, applied to async_accept().
  util::Pipe_reader m_ready_reader;

  /// Similar to the one in Session_adapter, applied to async_accept().
  util::Pipe_writer m_ready_writer;

  /// Similar to the one in Session_adapter, applied to async_accept().
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl;

  /// Similar to the one in Session_adapter.
  util::sync_io::Event_wait_func m_ev_wait_func;

  /// `on_done_func` from async_accept() if one is pending; otherwise `.empty()`.
  flow::async::Task_asio_err m_on_done_func_or_empty;

  /// Result given to (or about to be given to) #m_on_done_func_or_empty.
  Error_code m_target_err_code;

  /// This guy does all the work.  In our dtor this will be destroyed (hence thread stopped) first-thing.
  Async_io_obj m_async_io;
}; // class Session_server_adapter

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Session_server>
template<typename... Ctor_args>
Session_server_adapter<Session_server>::Session_server_adapter(Ctor_args&&... ctor_args) :
  m_ready_reader(m_nb_task_engine), // No handle inside but will be set-up soon below.
  m_ready_writer(m_nb_task_engine), // Ditto.
  m_ev_wait_hndl(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  m_async_io(std::forward<Ctor_args>(ctor_args)...) // That had to have set up get_logger(), etc., by the way.
{
  using util::Native_handle;
  using boost::asio::connect_pipe;

  Error_code sys_err_code;

  connect_pipe(m_ready_reader, m_ready_writer, sys_err_code);
  if (sys_err_code)
  {
    FLOW_LOG_FATAL("Session_server_adapter [" << *this << "]: Constructing: connect-pipe failed.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_FATAL();
    assert(false && "We chose not to complicate the code given how unlikely this is, and how hosed you'd have to be.");
    std::abort();
  }

  m_ev_wait_hndl.assign(Native_handle(m_ready_reader.native_handle()));
}

template<typename Session_server>
template<typename Event_wait_func_t>
bool Session_server_adapter<Session_server>::start_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::Task;

  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Session_server_adapter [" << *this << "]: Start-ops requested, "
                     "but we are already started.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else

  m_ev_wait_func = std::move(ev_wait_func);

  FLOW_LOG_INFO("Session_server_adapter [" << *this << "]: Start-ops requested.  Done.");
  return true;

  // That's it for now.  async_accept() will start an actual async-wait.
} // Session_adapter::start_ops()

template<typename Session_server>
template<typename Create_ev_wait_hndl_func>
bool Session_server_adapter<Session_server>::replace_event_wait_handles
       (const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  using util::Native_handle;

  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Session_server_adapter [" << *this << "]: Cannot replace event-wait handles after "
                     "a start-*-ops procedure has been executed.  Ignoring.");
    return false;
  }
  // else

  FLOW_LOG_INFO("Session_server_adapter [" << *this << "]: "
                "Replacing event-wait handles (probably to replace underlying "
                "execution context without outside event loop's boost.asio Task_engine or similar).");

  assert(m_ev_wait_hndl.is_open());

  Native_handle saved(m_ev_wait_hndl.release());
  m_ev_wait_hndl = create_ev_wait_hndl_func();
  m_ev_wait_hndl.assign(saved);

  return true;
} // Session_server_adapter::replace_event_wait_handles()

template<typename Session_server>
template<typename Task_err>
bool Session_server_adapter<Session_server>::async_accept(Session_obj* target_session, Task_err&& on_done_func)
{
  using util::Task;

  if (!m_on_done_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("Session_server_adapter [" << *this << "]: "
                     "Async-accept requested during async-accept.  Ignoring.");
    return false;
  }
  // else
  m_on_done_func_or_empty = std::move(on_done_func);

  core()->async_accept(target_session->core(), // <-- ATTN!  It's fine, because we don't do core()->init_handlers(). -*-
                       [this](const Error_code& err_code) { accept_write(err_code); });
  // -*- They will need to do target_session->init_handlers() (analogously to vanilla would-be core()->init_handlers()).

  m_ev_wait_func(&m_ev_wait_hndl,
                 false, // Wait for read.
                 boost::make_shared<Task>([this]() { accept_read(); }));

  return true;
} // Session_server_adapter::async_accept()

template<typename Session_server>
template<typename Task_err,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
bool Session_server_adapter<Session_server>::async_accept
       (Session_obj* target_session,
        typename Session_server_obj::Channels* init_channels_by_srv_req,
        typename Session_server_obj::Mdt_reader_ptr* mdt_from_cli_or_null,
        typename Session_server_obj::Channels* init_channels_by_cli_req,
        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
        Mdt_load_func&& mdt_load_func,
        Task_err&& on_done_func)
{
  using util::Task;

  if (!m_on_done_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("Session_server_adapter [" << *this << "]: "
                     "Async-accept requested during async-accept.  Ignoring.");
    return false;
  }
  // else
  m_on_done_func_or_empty = std::move(on_done_func);

  core()->async_accept(target_session->core(), // <-- ATTN!  It's fine, because we don't do core()->init_handlers(). -*-
                       init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                       std::move(n_init_channels_by_srv_req_func), std::move(mdt_load_func),
                       [this](const Error_code& err_code) { accept_write(err_code); });
  // -*- They will need to do target_session->init_handlers() (analogously to vanilla would-be core()->init_handlers()).

  m_ev_wait_func(&m_ev_wait_hndl,
                 false, // Wait for read.
                 boost::make_shared<Task>([this]() { accept_read(); }));

  return true;
} // Session_server_adapter::async_accept()

template<typename Session_server>
void Session_server_adapter<Session_server>::accept_write(const Error_code& err_code)
{
  if (err_code == error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
  {
    return; // Stuff is shutting down.  GTFO.
  }
  // else

  FLOW_LOG_INFO("Session_server_adapter [" << *this << "]: Async-IO core reports accept-complete event: "
                "tickling IPC-pipe to inform user.");

  m_target_err_code = err_code;

  util::pipe_produce(get_logger(), &m_ready_writer);
}

template<typename Session_server>
void Session_server_adapter<Session_server>::accept_read()
{
  FLOW_LOG_INFO("Session_server_adapter [" << *this << "]: Async-IO core accept-complete event: "
                "informed via IPC-pipe; invoking handler.");
  util::pipe_consume(get_logger(), &m_ready_reader); // They could in theory try again, if that actually failed.

  auto on_done_func = std::move(m_on_done_func_or_empty);
  m_on_done_func_or_empty.clear(); // In case move() didn't do it.

  on_done_func(m_target_err_code);
  FLOW_LOG_TRACE("Handler completed.");
}

template<typename Session_server>
typename Session_server_adapter<Session_server>::Session_server_obj*
  Session_server_adapter<Session_server>::core()
{
  return &m_async_io;
}

template<typename Session_server>
const typename Session_server_adapter<Session_server>::Session_server_obj*
  Session_server_adapter<Session_server>::core() const
{
  return const_cast<Session_server_adapter*>(this)->core();
}

template<typename Session_server>
flow::log::Logger* Session_server_adapter<Session_server>::get_logger() const
{
  return core()->get_logger();
}

template<typename Session_server>
const flow::log::Component& Session_server_adapter<Session_server>::get_log_component() const
{
  return core()->get_log_component();
}

template<typename Session_server>
std::ostream& operator<<(std::ostream& os,
                         const Session_server_adapter<Session_server>& val)
{
  return os << "SIO@" << static_cast<const void*>(&val) << " sess_srv[" << (*(val.core())) << ']';
}

} // namespace ipc::session::sync_io
