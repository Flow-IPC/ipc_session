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
 * `sync_io`-pattern counterpart to async-I/O-pattern session::Client_session types and all their SHM-aware variations
 * (at least shm::classic::Client_session et al and shm::arena_lend::jemalloc::Client_session et al).  In point of fact:
 *   - Use this if and only if you desire a `sync_io`-pattern style of being informed of async events from a
 *     `Client_session` of any kind.  For example, you may find this convenient if your event loop is an old-school
 *     reactor using `poll()` or `epoll_wait()`.  This affects exactly the following APIs:
 *     - `Client_session::async_connect()` reporting that a session-open attempt has completed.
 *     - `Client_session` reporting a session-hosing error via on-error handler.
 *       - Set up via ctor.
 *     - `Client_session` reporting a channel having been passively-opened via that handler.
 *       - Set up via ctor.
 *   - This Client_session_adapter *adapts* a `Client_session` constructed and stored within `*this`.
 *     All APIs excluding the above -- that is to say all non-async APIs -- are to be invoked via core() accessor.
 *     - Trying to use `core()->async_connect()` leads to undefined behavior.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 * @see session::Client_session_mv, session::Client_session, et al.
 *
 * As is generally the case when choosing `sync_io::X` versus `X`, we recommend using `X` due to it being easier.
 * In this particular case (see below) there is no perf benefit to using `sync_io::X`, either, so the only reason
 * to use `sync_io::X` in this case would be because you've got an old-school reactor event loop with
 * a `poll()` or `epoll_wait()`, in which case the `sync_io` API may be easier to integrate.
 *
 * To use it:
 *   - Construct it explicitly.  As with an async-I/O `Client_session` you will need to provide:
 *     - Error handler (though it will be invoked synchronously per `sync_io` pattern).
 *     - (Optional -- if you want to enable channel passive-open): Channel passive-open handler
 *       (though it will be invoked via... ditto).
 *   - Set up `sync_io` pattern using start_ops() (and if needed precede it with replace_event_wait_handles()).
 *   - Use async_connect() in similar fashion to async-I/O `Client_session` (though it will be invoked via... ditto).
 *   - On successful connect:
 *     - Be ready for error handler to fire (in `sync_io` style).
 *     - Be ready for passive-channel-open handler to fire (in `sync_io` style).
 *     - Use `core()->` for all other API needs (e.g., `open_channel()`, `session_shm()` -- if SHM-aware).
 *
 * ### Internal implementation ###
 * Normally this would not be in the public docs for this public-use class, but indulge us.
 *
 * In perf-critical situations, such as the various transport::Blob_sender / transport::Blob_receiver / etc.
 * impls, typically `sync_io::X` contains the core implementation, then `X` adapts a `sync_io::X` *core*
 * to provide background-thread-driven work and signaling of completion.  We do not consider
 * the present `X = Client_session` to be perf-critical; as such the present `sync_io::X` adapts
 * the `X`.  Internally this is accomplished using an unnamed IPC-pipe, where
 * an internal background thread W tickles said IPC-pipe which is waited-on by the user of
 * sync_io::Client_session_adapter.
 *
 * @todo Make all of Server_session_adapter, Client_session_adapter move-ctible/assignable like their adapted
 * counterparts.  It is not of utmost importance practically, unlike for the adapter guys, but at least for
 * consistency it would be good; and of course it never hurts usability even if not critical.
 * (Internally: This is not difficult to implement; the async-I/O guys being movable was really the hard part.)
 *
 * @tparam Session
 *         The async-I/O `Client_session` concrete type being adapted.  As of this writing that would be one of
 *         at least: `session::Client_session<knobs>`, `session::shm::classic::Client_session<knobs>`,
 *         `session::shm::jemalloc::Client_session<knobs>`.
 */
template<typename Session>
class Client_session_adapter :
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

  /**
   * Forwards to the #Session_obj ctor.  See Client_session_mv ctor doc headers.
   *
   * @tparam Ctor_args
   *         See above.
   * @param ctor_args
   *        See above.
   */
  template<typename... Ctor_args>
  Client_session_adapter(Ctor_args&&... ctor_args);

  /// Destructor.
  ~Client_session_adapter();

  // Methods.

  /**
   * Sets up the `sync_io`-pattern interaction between `*this` and the user's event loop; required before
   * `*this` will do any work.
   *
   * `ev_wait_func()` -- with signature matching util::sync_io::Event_wait_func -- is a key function memorized
   * by `*this`.  It shall be invoked by `*this` operations when some op cannot complete synchronously and requires
   * a certain event (readable/writable) to be active on a certain native-handle.
   *
   * @see util::sync_io::Event_wait_func doc header for useful and complete instructions on how to write an
   *      `ev_wait_func()` properly.  Doing so correctly is the crux of using the `sync_io` pattern.
   *
   * This is a standard `sync_io`-pattern API per util::sync_io doc header.
   *
   * @tparam Event_wait_func_t
   *         Function type matching util::sync_io::Event_wait_func.
   * @param ev_wait_func
   *        See above.
   * @return `false` if this has already been invoked; no-op logging aside.  `true` otherwise.
   */
  template<typename Event_wait_func_t>
  bool start_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Analogous to transport::sync_io::Native_handle_sender::replace_event_wait_handles().
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
   * Acts identically to 1-arg overload of Client_session_mv::async_connect(), except that the completion handler
   * is invoked in the `sync_io`-pattern fashion, synchronously inside an async-wait performed by you and
   * reported via `(*on_active_ev_func)()`.  Returns `false` if invoked before start_ops() in addition to the
   * possible reasons per Client_session_mv::async_connect().
   *
   * @tparam Task_err
   *         See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err>
  bool async_connect(Task_err&& on_done_func);

  /**
   * Acts identically to 5-arg overload of Client_session_mv::async_connect(), except that the completion handler
   * is invoked in the `sync_io`-pattern fashion, synchronously inside an async-wait performed by you and
   * reported via `(*on_active_ev_func)()`.  Returns `false` if invoked before start_ops() in addition to the
   * possible reasons per Client_session_mv::async_connect().
   *
   * @tparam Task_err
   *         See above.
   * @param mdt
   *        See above.
   * @param init_channels_by_cli_req_pre_sized
   *        See above.
   * @param mdt_from_srv_or_null
   *        See above.
   * @param init_channels_by_srv_req
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.
   */
  template<typename Task_err>
  bool async_connect(const typename Session_obj::Mdt_builder_ptr& mdt,
                     typename Session_obj::Channels* init_channels_by_cli_req_pre_sized,
                     typename Session_obj::Mdt_reader_ptr* mdt_from_srv_or_null,
                     typename Session_obj::Channels* init_channels_by_srv_req,
                     Task_err&& on_done_func);

  /**
   * The adapted mutable #Session_obj.  It is safe to access any API except for `core()->async_connect()` (undefined
   * behavior); use `this->async_connect()` instead.  Remember that start_ops() is required first.
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

private:
  // Methods.

  /**
   * The real handler given for `on_done_func` to `Session_obj::async_connect()`: it records the
   * result of that async-connect to #m_target_err_code_conn, then signals conn_read() via
   * the IPC-pipe.
   *
   * @param err_code
   *        Result from `Session_obj::async_connect()`.
   */
  void conn_write(const Error_code& err_code);

  /**
   * Signaled by conn_write(), it returns the IPC-pipe to steady-state (empty, not readable), then invokes
   * the original user `on_done_func()`.
   */
  void conn_read();

  // Data.

  /// Similar to the one in #Base.
  flow::util::Task_engine m_nb_task_engine;

  /// Similar to the one in #Base.
  flow::util::Task_engine m_ev_hndl_task_engine_unused;

  /// Similar to the ones in #Base, applied to async_connect().
  util::Pipe_reader m_ready_reader_conn;

  /// Similar to the ones in #Base, applied to async_connect().
  util::Pipe_writer m_ready_writer_conn;

  /// Similar to the ones in #Base, applied to async_connect().
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_conn;

  /// `on_done_func` from async_connect() if one is pending; otherwise `.empty()`.
  flow::async::Task_asio_err m_on_conn_func_or_empty;

  /// Result given to (or about to be given to) #m_on_conn_func_or_empty.
  Error_code m_target_err_code_conn;
}; // class Client_session_adapter

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Session>
template<typename... Ctor_args>
Client_session_adapter<Session>::Client_session_adapter(Ctor_args&&... ctor_args) :
  Base(std::forward<Ctor_args>(ctor_args)...), // That had to have set up get_logger(), etc., by the way.

  m_ready_reader_conn(m_nb_task_engine), // No handle inside but will be set-up soon below.
  m_ready_writer_conn(m_nb_task_engine), // Ditto.
  m_ev_wait_hndl_conn(m_ev_hndl_task_engine_unused) // This needs to be .assign()ed still.
{
  Base::init_pipe(&m_ready_reader_conn, &m_ready_writer_conn, &m_ev_wait_hndl_conn);
}

template<typename Session>
Client_session_adapter<Session>::~Client_session_adapter()
{
  Base::dtor_stop(); // Stop the Client_session, and its thread W, as various this->m_* items are about to disappear.
}

template<typename Session>
template<typename Event_wait_func_t>
bool Client_session_adapter<Session>::start_ops(Event_wait_func_t&& ev_wait_func)
{
  return Base::start_ops(std::move(ev_wait_func));

  // That's it for now.  async_connect() will start an actual async_wait().
} // Session_adapter::start_ops()

template<typename Session>
template<typename Create_ev_wait_hndl_func>
bool
  Client_session_adapter<Session>::replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  using util::Native_handle;

  if (!Base::replace_event_wait_handles(create_ev_wait_hndl_func))
  {
    return false; // Early call.  It logged.
  }
  // else

  assert(m_ev_wait_hndl_conn.is_open());

  Native_handle saved(m_ev_wait_hndl_conn.release());
  m_ev_wait_hndl_conn = create_ev_wait_hndl_func();
  m_ev_wait_hndl_conn.assign(saved);

  return true;
} // Client_session_adapter::replace_event_wait_handles()

template<typename Session>
template<typename Task_err>
bool Client_session_adapter<Session>::async_connect(Task_err&& on_done_func)
{
  using util::Task;

  if (!m_on_conn_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("Client_session_adapter [" << *this << "]: "
                     "Async-connect requested during async-connect.  Ignoring.");
    return false;
  }
  // else
  m_on_conn_func_or_empty = std::move(on_done_func);

  if (!core()->async_connect([this](const Error_code& err_code) { conn_write(err_code); }))
  {
    m_on_conn_func_or_empty.clear(); // Undo.
    return false; // Connect while connected, or something.  It logged.
  }
  // else

  Base::async_wait(&m_ev_wait_hndl_conn,
                   false, // Wait for read.
                   boost::make_shared<Task>([this]() { conn_read(); }));

  return true;
} // Client_session_adapter::async_connect()

template<typename Session>
template<typename Task_err>
bool Client_session_adapter<Session>::async_connect
       (const typename Session_obj::Mdt_builder_ptr& mdt,
        typename Session_obj::Channels* init_channels_by_cli_req_pre_sized,
        typename Session_obj::Mdt_reader_ptr* mdt_from_srv_or_null,
        typename Session_obj::Channels* init_channels_by_srv_req,
        Task_err&& on_done_func)
{
  using util::Task;

  if (!m_on_conn_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("Client_session_adapter [" << *this << "]: "
                     "Async-connect requested during async-connect.  Ignoring.");
    return false;
  }
  // else
  m_on_conn_func_or_empty = std::move(on_done_func);

  const bool ok = core()->async_connect(mdt, init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null,
                                        init_channels_by_srv_req,
                                        [this](const Error_code& err_code)
                                          { conn_write(err_code); });
  if (!ok)
  {
    m_on_conn_func_or_empty.clear(); // Undo.
    return false;
  }
  // else

  Base::async_wait(&m_ev_wait_hndl_conn,
                   false, // Wait for read.
                   boost::make_shared<Task>([this]() { conn_read(); }));

  return true;
} // Client_session_adapter::async_connect()

template<typename Session>
void Client_session_adapter<Session>::conn_write(const Error_code& err_code)
{
  if (err_code == error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
  {
    return; // Stuff is shutting down.  GTFO.
  }
  // else

  FLOW_LOG_INFO("Client_session_adapter [" << *this << "]: Async-IO core reports connect-complete event: "
                "tickling IPC-pipe to inform user.");

  m_target_err_code_conn = err_code;

  util::pipe_produce(get_logger(), &m_ready_writer_conn);
}

template<typename Session>
void Client_session_adapter<Session>::conn_read()
{
  FLOW_LOG_INFO("Client_session_adapter [" << *this << "]: Async-IO core connect-complete event: "
                "informed via IPC-pipe; invoking handler.");
  util::pipe_consume(get_logger(), &m_ready_reader_conn); // They could in theory try again, if that actually failed.

  auto on_done_func = std::move(m_on_conn_func_or_empty);
  m_on_conn_func_or_empty.clear(); // In case move() didn't do it.

  on_done_func(m_target_err_code_conn);
  FLOW_LOG_TRACE("Handler completed.");
}

template<typename Session>
typename Client_session_adapter<Session>::Session_obj*
  Client_session_adapter<Session>::core()
{
  return Base::core();
}

template<typename Session>
const typename Client_session_adapter<Session>::Session_obj*
  Client_session_adapter<Session>::core() const
{
  return const_cast<Client_session_adapter*>(this)->core();
}

template<typename Session>
std::ostream& operator<<(std::ostream& os,
                         const Client_session_adapter<Session>& val)
{
  return os << "SIO@" << static_cast<const void*>(&val) << " cli_sess[" << (*(val.core())) << ']';
}

} // namespace ipc::session::sync_io
