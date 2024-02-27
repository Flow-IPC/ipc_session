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

#include "ipc/session/session_fwd.hpp"
#include "ipc/util/detail/util_fwd.hpp"
#include "ipc/util/sync_io/sync_io_fwd.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session::sync_io
{

// Types.

/* @todo Technically by the established conventions of this project, there should be a ..._fwd.hpp with
 * forward-declaration of `template<typename Session> class Session_adapter;`.  Maybe we should do that.  It's just...
 * exactly two things use it (Client_session_adapter, Server_session_adapter), and it's just such pointless
 * boiler-plate at the moment to make a sync_io/detail/sync_io_fwd.hpp just for that, when in reality they're
 * templates and are going to include the present file anyway.  Just, eh.  For now anyway.  It's just in most other
 * contexts postponing doing this just leads to needing to do it later anyway, at which point it feels more painful.
 * Here though... ehhhh.... */

/**
 * Internal-use workhorse containing common elements of Client_session_adapter and Server_session_adapter.
 * As such it contains the machinery for:
 *   - storing (deriving from) the adapted `Session` as accessible via Session_adapter::core();
 *   - adapting async-I/O on-error and on-passive-channel-open (if enabled) events to `sync_io` pattern;
 *   - storing and invoking the central util::sync_io::Event_wait_func supplied by the user;
 *   - type aliases common to all sessions.
 *
 * @tparam Session
 *         See, e.g., Client_session_adapter.
 */
template<typename Session>
class Session_adapter
{
public:
  // Types.

  /// See, e.g., Client_session_adapter.
  using Session_obj = Session;

  /// See, e.g., Client_session_adapter.
  using Sync_io_obj = transport::Null_peer;
  /// See, e.g., Client_session_adapter.
  using Async_io_obj = Session_obj;

  /// Short-hand for session-openable Channel type.
  using Channel_obj = typename Session_obj::Channel_obj;

  /// Short-hand for session-open metadata reader.
  using Mdt_reader_ptr = typename Session_obj::Mdt_reader_ptr;

  /// Short-hand for passive-channel-open handler.
  using On_channel_func = Function<void (Channel_obj&&, Mdt_reader_ptr&&)>;

  // Constructors/destructor.

  /**
   * Compilable only when #Session_obj is a `Client_session` variant, forwards to its ctor of identical form,
   * except that the handlers are replaced with `sync_io`-adapting ones.  Do not use init_handlers() after this ctor,
   * as that is for `Server_session`.
   *
   * @param logger_ptr
   *        See Client_session_mv.
   * @param cli_app_ref
   *        See Client_session_mv.
   * @param srv_app_ref
   *        See Client_session_mv.
   * @param on_err_func
   *        See Client_session_mv.  However in `*this` case this shall be invoked -- though still only following
   *        successful XXXsearch async_connect() as usual -- according to `sync_io` pattern, synchronously inside
   *        an async-wait performed by you and reported via `(*on_active_ev_func)()`.
   * @param on_passive_open_channel_func
   *        See Client_session_mv.  However in `*this` case this shall be invoked -- though still only following
   *        successful async_connect() as usual -- according to `sync_io` pattern, synchronously inside
   *        an async-wait performed by you and reported via `(*on_active_ev_func)()`.
   * @tparam On_passive_open_channel_handler
   *         See Client_session_mv.
   * @tparam Task_err
   *         See Client_session_mv.
   */
  template<typename On_passive_open_channel_handler, typename Task_err>
  explicit Session_adapter(flow::log::Logger* logger_ptr,
                           const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                           Task_err&& on_err_func,
                           On_passive_open_channel_handler&& on_passive_open_channel_func);

  /**
   * Compilable only when #Session_obj is a `Client_session` variant, forwards to its ctor of identical form,
   * except that the handler is replaced with `sync_io`-adapting one.  Do not use init_handlers() after this ctor,
   * as that is for `Server_session`.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param cli_app_ref
   *        See Client_session_mv.
   * @param srv_app_ref
   *        See Client_session_mv.
   * @param on_err_func
   *        See Client_session_mv.  However in `*this` case this shall be invoked -- though still only following
   *        successful async_connect() as usual -- according to `sync_io` pattern, synchronously inside
   *        an async-wait performed by you and reported via `(*on_active_ev_func)()`.
   * @tparam Task_err
   *         See Client_session_mv.
   */
  template<typename Task_err>
  explicit Session_adapter(flow::log::Logger* logger_ptr,
                           const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                           Task_err&& on_err_func);

  /// Forwards to the #Session_obj default ctor.
  Session_adapter();

  /**
   * See, e.g., Client_session_adapter.
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
   * See, e.g., Client_session_adapter.
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
   * Compilable only when #Session_obj is a `Server_session` variant, forwards to its method of identical form,
   * except that the handlers are replaced with `sync_io`-adapting ones.  Do not use non-default ctor before this,
   * as that is for `Client_session`.
   *
   * @tparam Task_err
   *         See Session_server.
   * @tparam On_passive_open_channel_handler
   *         See Session_server.
   * @param on_err_func_arg
   *        See Session_server.
   * @param on_passive_open_channel_func_arg
   *        See Session_server.
   * @return See Session_server; in addition returns `false`/no-ops if invoked before start_ops().
   */
  template<typename Task_err, typename On_passive_open_channel_handler>
  bool init_handlers(Task_err&& on_err_func_arg, On_passive_open_channel_handler&& on_passive_open_channel_func_arg);

  /**
   * Compilable only when #Session_obj is a `Server_session` variant, forwards to its method of identical form,
   * except that the handlers are replaced with `sync_io`-adapting ones.  Do not use non-default ctor before this,
   * as that is for `Client_session`.
   *
   * @tparam Task_err
   *         See Session_server.
   * @param on_err_func_arg
   *        See Session_server.
   * @return See Session_server; in addition returns `false`/no-ops if invoked before start_ops().
   */
  template<typename Task_err>
  bool init_handlers(Task_err&& on_err_func_arg);

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

protected:
  // Methods.

  /**
   * Forwards to the util::sync_io::Event_wait_func saved in start_ops().
   *
   * @tparam Args
   *         See above.
   * @param args
   *        See above.
   */
  template<typename... Args>
  void async_wait(Args&&... args);

  /**
   * Utility that sets up an IPC-pipe in the given peer objects as well as loading a watcher-descriptor
   * object for the read-end.  This should be called before start_ops() or replace_event_wait_handles().
   *
   * @param reader
   *        Pipe read end to load.  It should be constructed with its intended execution context/executor but
   *        not `.is_open()`.
   * @param writer
   *        Pipe write end to load.  It should be constructed with its intended execution context/executor but
   *        not `.is_open()`.
   * @param ev_wait_hndl
   *        Watcher for `reader`.  It should be constructed with its intended execution context/executor but
   *        not `.is_open()`.
   */
  void init_pipe(util::Pipe_reader* reader, util::Pipe_writer* writer,
                 util::sync_io::Asio_waitable_native_handle* ev_wait_hndl);

  /**
   * The adapted mutable #Session_obj.
   * @return See above.
   */
  Session_obj* core();

  /**
   * The adapted mutable #Session_obj.
   * @return See above.
   */
  const Session_obj* core() const;

private:
  // Types.

  /// Set of result arg values from a successful passive-channel-open from a #Session_obj invoking #On_channel_func.
  struct Channel_open_result
  {
    // Types.

    /// Short-hand for pointer wrapper around a `*this`.
    using Ptr = boost::movelib::unique_ptr<Channel_open_result>;

    // Data.

    /// Result 1/2 given about to be given to #m_on_channel_func_or_empty.
    Channel_obj m_channel;

    /// Result 2/2 given about to be given to #m_on_channel_func_or_empty.
    Mdt_reader_ptr m_mdt_reader_ptr;
  };

  /// Queue of Channel_open_result.
  using Channel_open_result_q = std::queue<typename Channel_open_result::Ptr>;

  // Methods.

  /**
   * Signaled by the function returned by on_channel_func_sio(), it returns the IPC-pipe to steady-state (empty,
   * not readable), invokes the original user handler passed to on_channel_func_sio(), and lastly
   * begins the next async-wait for the procedure.
   */
  void on_ev_channel_open();

  /**
   * Returns the proper on-error handler to set up on the underlying #Session_obj (`Client_session`:
   * via ctor; `Server_session`: via `init_handlers()`).
   *
   * The resulting handler must not be invoked before start_ops() and #m_on_err_func being set;
   * else behavior undefined.
   *
   * @return See above.
   */
  flow::async::Task_asio_err on_err_func_sio();

  /**
   * Returns the proper on-passive-channel-open handler to set up on the underlying #Session_obj (`Client_session`:
   * via ctor; `Server_session`: via `init_handlers()` 2-arg form).
   *
   * The resulting handler must not be invoked before start_ops() and #m_on_channel_func_or_empty being set;
   * else behavior undefined.
   *
   * @return See above.
   */
  On_channel_func on_channel_func_sio();

  // Data.

  /**
   * The `Task_engine` for `m_ready_*`.  It is necessary to construct those pipe-end objects, but we never
   * use that guy's `->async_*()` APIs -- only non-blocking operations, essentially leveraging boost.asio's
   * portable transmission APIs but not its actual, um, async-I/O abilities in this case.  Accordingly we
   * never load any tasks onto #m_nb_task_engine and certainly never `.run()` (or `.poll()` or ...) it.
   *
   * In the `sync_io` pattern the user's outside event loop is responsible for awaiting readability/writability
   * of a guy like #m_ready_reader_chan via our exporting of its `.native_handle()`.
   */
  flow::util::Task_engine m_nb_task_engine;

  /**
   * The `Task_engine` for `m_ev_wait_hndl_*`, unless it is replaced via replace_event_wait_handles().
   *
   * This is to fulfill the `sync_io` pattern.
   */
  flow::util::Task_engine m_ev_hndl_task_engine_unused;

  /**
   * Read-end of IPC-pipe used by `*this` used to detect that the error-wait has completed.  The signal byte
   * is detected in #m_ready_reader_err.  There is no need to read it, as an error can occur at most once.
   *
   * @see #m_ready_writer_err.
   */
  util::Pipe_reader m_ready_reader_err;

  /// Write-end of IPC-pipe together with #m_ready_reader_err.
  util::Pipe_writer m_ready_writer_err;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_ready_reader_err.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_err;

  /**
   * Read-end of IPC-pipe used by `*this` used to detect that a channel-open-wait has completed.  The signal byte
   * is read out of #m_ready_reader_chan, after it was written there via #m_ready_writer_chan.  As explained in
   * #m_target_channel_open_q doc header, channel-opens can occur at any time from another thread, even while
   * `*this` is synchronously dealing with an existing one; therefore this is a rare case where more than 1 byte
   * could be readable at a given time.  Nevertheless we issue one async-wait on #m_ready_reader_chan per
   * channel-open, in series; eventually all of them get popped, meaning all the channel-opens have been served.
   *
   * This is not a rare occurrence necessarily: an opposing `Session` user can perform 2+ `open_channel()` right in
   * a row, and since creating a `Channel` with all its 2-4 fat peer objects can be relatively lengthy on this
   * side, the 2nd/3rd/... on-channel-open firing can "step on" a preceding one.
   *
   * @see #m_ready_writer_chan.
   */
  util::Pipe_reader m_ready_reader_chan;

  /// Write-end of IPC-pipe together with #m_ready_reader_chan.
  util::Pipe_writer m_ready_writer_chan;

  /**
   * Descriptor waitable by outside event loop async-waits -- storing the same `Native_handle` as (and thus being
   * used to wait on events from) #m_ready_reader_chan.
   */
  util::sync_io::Asio_waitable_native_handle m_ev_wait_hndl_chan;

  /**
   * Function (set forever in start_ops()) through which we invoke the outside event loop's
   * async-wait facility for descriptors/events relevant to our ops.  See util::sync_io::Event_wait_func
   * doc header for a refresher on this mechanic.
   */
  util::sync_io::Event_wait_func m_ev_wait_func;

  /// `on_err_func` from init_handlers(); `.empty()` until then.
  flow::async::Task_asio_err m_on_err_func;

  /**
   * `on_passive_open_channel_func_or_empty` from init_handlers() (possibly `.empty()` if not supplied); until then
   * `.empty()`.
   */
  On_channel_func m_on_channel_func_or_empty;

  /// Result given to (or about to be given to) #m_on_err_func.
  Error_code m_target_err_code_err;

  /**
   * Queue of #On_channel_func handler arg sets received from async-I/O #Session_obj -- meaning
   * the `Session`, in unspecified background thread, informing us (which we signal via #m_ready_writer_chan)
   * a channel has been passively open -- and not yet fed to #m_on_channel_func_or_empty.
   *
   * Protected by #m_target_channel_open_q_mutex.
   *
   * ### Rationale ###
   * There's a queue here, which stands in contrast to (as of this writing) all other `sync_io` pattern impls
   * in the project, including the on-error stuff in `*this`: those always deal with a single async-op at a time,
   * and therefore no queue is necessary (just a single arg-set; usually just an `Error_code`, sometimes
   * with a `size_t sz` though).
   *
   * The reason this one is different is as follows.
   *   - It's unlike, e.g., Client_session_adapter::async_connect()XXX, because an async-connect is triggered by the
   *     user explicitly, which triggers a single async-wait, and we disallow in our API to trigger more, until
   *     that one completes.
   *   - It's unlike, e.g., #m_on_err_func stuff in `*this`, because while that one can indeed happen at any time
   *     from another thread -- without the user explicitly beginning the async-op (it sort of begins by itself) --
   *     it can also only happen at most once per `*this` (modulo move-assignment).
   *
   * At any rate: the handler we register with #m_async_io -- on_channel_func_sio() -- can be invoked
   * from a background thread at any time.  This triggers signaling #m_ready_reader_chan (via #m_ready_writer_chan),
   * on which there is always a `sync_io`-pattern async-wait outstanding.  We are informed of a completed
   * async-wait (byte is available on #m_ready_reader_chan); we consume the byte and pop and pass the result
   * from top of #m_target_channel_open_q.  Then we start another async-wait.  If there are more items in the queue
   * that were added in the meantime, the same number of bytes have been pushed into #m_ready_writer_chan;
   * so those async-waits are satisfied (in series) quickly, leading to the progressive emptying of this queue
   * and passing along those results to `m_on_channel_func_or_empty()`.
   */
  Channel_open_result_q m_target_channel_open_q;

  /// Protects #m_target_channel_open_q, accessed from user async-wait-reporter thread; and #Session_obj worker thread.
  mutable flow::util::Mutex_non_recursive m_target_channel_open_q_mutex;

  /// This guy does all the work.  In our dtor this will be destroyed (hence thread stopped) first-thing.
  Async_io_obj m_async_io;
}; // class Session_adapter

// Template implementations.

template<typename Session>
Session_adapter<Session>::Session_adapter() :
  m_ready_reader_err(m_nb_task_engine), // No handle inside but will be set-up soon below.
  m_ready_writer_err(m_nb_task_engine), // Ditto.
  m_ev_wait_hndl_err(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  m_ready_reader_chan(m_nb_task_engine),
  m_ready_writer_chan(m_nb_task_engine),
  m_ev_wait_hndl_chan(m_ev_hndl_task_engine_unused)
{
  init_pipe(&m_ready_reader_err, &m_ready_writer_err, &m_ev_wait_hndl_err);
  init_pipe(&m_ready_reader_chan, &m_ready_writer_chan, &m_ev_wait_hndl_chan);
}

template<typename Session>
template<typename On_passive_open_channel_handler, typename Task_err>
Session_adapter<Session>::Session_adapter(flow::log::Logger* logger_ptr,
                                          const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                          Task_err&& on_err_func,
                                          On_passive_open_channel_handler&& on_passive_open_channel_func) :
  m_ready_reader_err(m_nb_task_engine), // No handle inside but will be set-up soon below.
  m_ready_writer_err(m_nb_task_engine), // Ditto.
  m_ev_wait_hndl_err(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  m_ready_reader_chan(m_nb_task_engine),
  m_ready_writer_chan(m_nb_task_engine),
  m_ev_wait_hndl_chan(m_ev_hndl_task_engine_unused),
  m_on_err_func(std::move(on_err_func)),
  m_on_channel_func_or_empty(std::move(on_passive_open_channel_func)),
  m_async_io(logger_ptr, cli_app_ref, srv_app_ref, on_err_func_sio(), on_channel_func_sio())
{
  init_pipe(&m_ready_reader_err, &m_ready_writer_err, &m_ev_wait_hndl_err);
  init_pipe(&m_ready_reader_chan, &m_ready_writer_chan, &m_ev_wait_hndl_chan);
}

template<typename Session>
template<typename Task_err>
Session_adapter<Session>::Session_adapter(flow::log::Logger* logger_ptr,
                                          const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                          Task_err&& on_err_func) :
  m_ready_reader_err(m_nb_task_engine), // No handle inside but will be set-up soon below.
  m_ready_writer_err(m_nb_task_engine), // Ditto.
  m_ev_wait_hndl_err(m_ev_hndl_task_engine_unused), // This needs to be .assign()ed still.
  m_ready_reader_chan(m_nb_task_engine),
  m_ready_writer_chan(m_nb_task_engine),
  m_ev_wait_hndl_chan(m_ev_hndl_task_engine_unused),
  m_on_err_func(std::move(on_err_func)),
  m_async_io(logger_ptr, cli_app_ref, srv_app_ref, on_err_func_sio())
{
  init_pipe(&m_ready_reader_err, &m_ready_writer_err, &m_ev_wait_hndl_err);
  init_pipe(&m_ready_reader_chan, &m_ready_writer_chan, &m_ev_wait_hndl_chan);
}

template<typename Session>
template<typename Task_err, typename On_passive_open_channel_handler>
bool Session_adapter<Session>::init_handlers(Task_err&& on_err_func_arg,
                                             On_passive_open_channel_handler&& on_passive_open_channel_func_arg)
{
  if (!m_on_err_func.empty())
  {
    FLOW_LOG_WARNING("Session_adapter [" << m_async_io << "]: init_handlers() called duplicately.  Ignoring.");
    return false;
  }
  // else
  assert(m_on_channel_func_or_empty.empty());

  m_on_err_func = std::move(on_err_func_arg);
  m_on_channel_func_or_empty = std::move(on_passive_open_channel_func_arg);

#ifndef NDEBUG
  const bool ok =
#endif
  core()->init_handlers(on_err_func_sio(), on_channel_func_sio());

  assert(ok && "We should have caught this with the above guard.");
  return true;
} // Session_adapter::init_handlers()

template<typename Session>
template<typename Task_err>
bool Session_adapter<Session>::init_handlers(Task_err&& on_err_func_arg)
{
  if (!m_on_err_func.empty())
  {
    FLOW_LOG_WARNING("Session_adapter [" << m_async_io << "]: init_handlers() called duplicately.  Ignoring.");
    return false;
  }
  // else

  m_on_err_func = std::move(on_err_func_arg);

#ifndef NDEBUG
  const bool ok =
#endif
  core()->init_handlers(on_err_func_sio());

  assert(ok && "We should have caught this with the above guard.");
  return true;
} // Session_adapter::init_handlers()

template<typename Session>
void Session_adapter<Session>::init_pipe(util::Pipe_reader* reader, util::Pipe_writer* writer,
                                         util::sync_io::Asio_waitable_native_handle* ev_wait_hndl)
{
  using util::Native_handle;
  using boost::asio::connect_pipe;

  Error_code sys_err_code;

  connect_pipe(*reader, *writer, sys_err_code);
  if (sys_err_code)
  {
    FLOW_LOG_FATAL("Session_adapter [" << m_async_io << "]: Constructing: connect-pipe failed.  Details follow.");
    FLOW_ERROR_SYS_ERROR_LOG_FATAL();
    assert(false && "We chose not to complicate the code given how unlikely this is, and how hosed you'd have to be.");
    std::abort();
  }

  ev_wait_hndl->assign(Native_handle(reader->native_handle()));
}

template<typename Session>
template<typename Event_wait_func_t>
bool Session_adapter<Session>::start_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::Task;

  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Session_adapter [" << m_async_io << "]: Start-ops requested, "
                     "but we are already started.  Probably a user bug, but it is not for us to judge.");
    return false;
  }
  // else

  m_ev_wait_func = std::move(ev_wait_func);

  /* No time to waste!  on_err_func_sio() and on_channel_func_sio() had to have been called already; begin waiting
   * for those events to be indicated via our IPC-pipes as set up in those methods. */

  async_wait(&m_ev_wait_hndl_err,
             false, // Wait for read.
             boost::make_shared<Task>([this]()
  {
    FLOW_LOG_INFO("Session_adapter [" << m_async_io << "]: Async-IO core on-error event: informed via IPC-pipe; "
                  "invoking handler.");
    util::pipe_consume(get_logger(), &m_ready_reader_err); // No need really -- it's a one-time thing -- but just....

    auto on_done_func = std::move(m_on_err_func);
    m_on_err_func.clear(); // In case move() didn't do it.  Might as well forget it.

    on_done_func(m_target_err_code_err);
    FLOW_LOG_TRACE("Handler completed.");
  }));

  /* Subtlety: It is tempting to guard this with `if (!m_on_channel_func_or_empty.empty()) {}` in the effort to
   * avoid an an async-wait that will never get satisfied.  However that only works in the Client_session case,
   * when m_on_channel_func_or_empty is known from ction.  Server_session still needs init_handlers() by user,
   * and indeed that would be after start_ops(), so => bug.  We could add fancier logic to account for this,
   * but who really cares?  So it'll register an extra FD in some boost.asio epoll-set.  Meh. */
  async_wait(&m_ev_wait_hndl_chan,
             false, // Wait for read.
             boost::make_shared<Task>([this]() { on_ev_channel_open(); }));

  FLOW_LOG_INFO("Session_adapter [" << m_async_io << "]: Start-ops requested.  Done.");
  return true;
} // Session_adapter::start_ops()

template<typename Session>
void Session_adapter<Session>::on_ev_channel_open()
{
  using util::Task;
  using flow::util::Lock_guard;

  // We are in user calling thread.

  typename Channel_open_result::Ptr result;
  {
    Lock_guard<decltype(m_target_channel_open_q_mutex)> lock(m_target_channel_open_q_mutex);

    FLOW_LOG_INFO("Session_adapter [" << m_async_io << "]: Async-IO core passively-opened channel event: "
                  "informed via IPC-pipe; invoking handler.  Including this one "
                  "[" << m_target_channel_open_q.size() << "] are pending.");

    assert((!m_target_channel_open_q.empty())
           && "Algorithm bug?  Result-queue elements and pipe signal bytes must be 1-1, so either something "
              "failed to correctly push, or something overzealously popped.");

    result = std::move(m_target_channel_open_q.front());
    m_target_channel_open_q.pop();
  } // Lock_guard lock(m_target_channel_open_q_mutex);

  m_on_channel_func_or_empty(std::move(result->m_channel), std::move(result->m_mdt_reader_ptr));
  FLOW_LOG_TRACE("Handler completed.  Beginning next async-wait immediately.  If more is/are pending "
                 "it/they'll be popped quickly due to immediately-completing async-wait(s).");

  util::pipe_consume(get_logger(), &m_ready_reader_chan);
  async_wait(&m_ev_wait_hndl_chan,
             false, // Wait for read -- again.
             boost::make_shared<Task>([this]() { on_ev_channel_open(); }));
} // Session_adapter::on_ev_channel_open()

template<typename Session>
template<typename Create_ev_wait_hndl_func>
bool Session_adapter<Session>::replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  using util::Native_handle;

  if (!m_ev_wait_func.empty())
  {
    FLOW_LOG_WARNING("Session_adapter [" << m_async_io << "]: Cannot replace event-wait handles after "
                     "a start-*-ops procedure has been executed.  Ignoring.");
    return false;
  }
  // else

  FLOW_LOG_INFO("Session_adapter [" << m_async_io << "]: Replacing event-wait handles (probably to replace underlying "
                "execution context without outside event loop's boost.asio Task_engine or similar).");

  assert(m_ev_wait_hndl_err.is_open());
  assert(m_ev_wait_hndl_chan.is_open());

  Native_handle saved(m_ev_wait_hndl_err.release());
  m_ev_wait_hndl_err = create_ev_wait_hndl_func();
  m_ev_wait_hndl_err.assign(saved);

  saved = m_ev_wait_hndl_chan.release();
  m_ev_wait_hndl_chan = create_ev_wait_hndl_func();
  m_ev_wait_hndl_chan.assign(saved);

  return true;
} // Session_adapter::replace_event_wait_handles()

template<typename Session>
flow::async::Task_asio_err Session_adapter<Session>::on_err_func_sio()
{
  using flow::async::Task_asio_err;

  // Careful in here!  *this may not have been constructed yet.

  return [this](const Error_code& err_code)
  {
    FLOW_LOG_INFO("Session_adapter [" << m_async_io << "]: Async-IO core reports on-error event: tickling IPC-pipe to "
                  "inform user.");

    assert((!m_target_err_code_err)
           && "Error handler must never fire more than once per Session!  Bug in the particular Session_obj type?");
    m_target_err_code_err = err_code;

    util::pipe_produce(get_logger(), &m_ready_writer_err);
  };

  // start_ops() will set up the (1-time) async-wait for m_ready_reader_err being readable.
} // Session_adapter::on_err_func_sio()

template<typename Session>
typename Session_adapter<Session>::On_channel_func
  Session_adapter<Session>::on_channel_func_sio()
{
  using flow::util::Lock_guard;

  // Careful in here!  *this may not have been constructed yet.

  return [this](Channel_obj&& new_channel, Mdt_reader_ptr&& new_channel_mdt)
  {
    // We are in Client_session_mv/Server_session_mv "unspecified" worker thread (called thread W in internal docs).
    {
      Lock_guard<decltype(m_target_channel_open_q_mutex)> lock(m_target_channel_open_q_mutex);

      FLOW_LOG_INFO("Session_adapter [" << m_async_io << "]: Async-IO core reports passively-opened channel event: "
                    "tickling IPC-pipe to inform user.  This will make the # of pending such events "
                    "[" << (m_target_channel_open_q.size() + 1) << "].");
      m_target_channel_open_q.emplace(boost::movelib::make_unique<Channel_open_result>());
      auto& result = *(m_target_channel_open_q.back());
      result.m_channel = std::move(new_channel);
      result.m_mdt_reader_ptr = std::move(new_channel_mdt);
    } // Lock_guard lock(m_target_channel_open_q_mutex);

    /* By Session contract, handlers are never called concurrently with each other.  Though this
     * in POSIX is thread-safe even otherwise (w/r/t another pipe_produce() and most certainly w/r/t
     * pipe_consume() of the other end). */
    util::pipe_produce(get_logger(), &m_ready_writer_chan);
  }; // return [](){}

  /* start_ops() will set up the (first) async-wait for m_ready_reader_err being readable, as well as
   * the action on that wait being satisfied; namely: to pipe_consume() that signal + begin the next wait. */
} // Session_adapter::on_channel_func_sio()

template<typename Session>
template<typename... Args>
void Session_adapter<Session>::async_wait(Args&&... args)
{
  m_ev_wait_func(std::forward<Args>(args)...);
}

template<typename Session>
typename Session_adapter<Session>::Session_obj* Session_adapter<Session>::core()
{
  return &m_async_io;
}

template<typename Session>
const typename Session_adapter<Session>::Session_obj* Session_adapter<Session>::core() const
{
  return const_cast<Session_adapter*>(this)->core();
}

template<typename Session>
flow::log::Logger* Session_adapter<Session>::get_logger() const
{
  return core()->get_logger();
}

template<typename Session>
const flow::log::Component& Session_adapter<Session>::get_log_component() const
{
  return core()->get_log_component();
}

} // namespace ipc::session::sync_io
