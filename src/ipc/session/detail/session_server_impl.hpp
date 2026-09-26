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

#include "ipc/session/detail/session_fwd.hpp"
#include "ipc/session/detail/server_session_dtl.hpp"
#include "ipc/session/error.hpp"
#include "ipc/session/app.hpp"
#include "ipc/transport/native_socket_stream_acceptor.hpp"
#include "ipc/transport/blob_stream_mq.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/util/detail/util.hpp"
#include <flow/util/util.hpp>
#include <cstddef>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/move/make_unique.hpp>
#include <boost/thread/condition_variable.hpp>

namespace ipc::session
{

// Types.

/**
 * Internal class template comprising API/logic common to every Session_server variant, meant to be
 * `private`ly sub-classed and largely forwarded.  In particular the vanilla Session_server
 * (see its short Implementation doc header section) sub-classes us and makes no use of the available
 * customization points.  Contrast with, e.g., shm::classic::Session_server which uses customization
 * points for shm::classic SHM-arena setup.
 *
 * The available customization points are as follows.
 *   - Per-session: `*this` is parameterized on #Server_session_obj.  The vanilla value is `Server_session<...>`;
 *     but to add capabilities extend Server_session_mv's behavior as explained in its doc header and proceed from
 *     there.  For example see shm::classic::Server_session.
 *     - No-customization = specify #Server_session here.
 *   - Cross-session: Additional per-Client_app setup (such as setting up cross-session SHM arena(s) in
 *     on-demand fashion) can be specified by passing in `per_app_setup_func` to ctor.
 *     - No-customization = specify do-nothing function here that returns `Error_code{}` always.
 *   - Cross-session: Certain custom code can be caused to run as the last thing in the Server_session_impl dtor,
 *     even after all its state has been destroyed (e.g., all threads have been stopped/joined).
 *     A sub-class may use this customization point by calling sub_class_set_deinit_func().  If not called, the
 *     customization point is unused by default.
 *
 * ### Implementation design ###
 * Generally the impl should be reasonably easy to follow by reading the method bodies, at least if one understands
 * the general ipc::session client-server paradigm.
 *
 * The thing to understand strategically, as usual, is the thread design.  This is an acceptor, so one might
 * have expected it to be implemented similarly to, say, Native_socket_stream_acceptor -- maintaining a thread W
 * in which to do some/most work; including maintaining a "deficit" queue of outstanding async_accept() requests
 * and a "surplus" queue of ready `Server_session`s to emit.  This was an option, but I (ygoldfel) felt that
 * piggy-backing handling of events directly onto the unspecified handler-invoking threads of the internally
 * used objects would produce a much simpler data structure and state machine.  (As a side effect, the behavior
 * described in the "FIFO" section of Session_server doc header occurs.  Also as a side effect, the error-emission
 * behavior described in its "Error handling" section occurs.  Basically: each async_accept()'s internal handling is
 * independent of the others.  They share (almost) no state; there is no single async-chain and explicit queues
 * unlike in various other boost.asio-like classes in ::ipc.  Each async_accept() triggers async op 1, the handler
 * for which triggers async op 2, the handler for which emits the result to user.  This is simpler to implement, but
 * it also results in sensible API contract behavior, I feel.)
 *
 * (Update: With hindsight from experience we added the following to-do.)
 *
 * @todo Session_server_impl (+ its subclasses, together as a whole) are internally complicated, with tricky
 * thread interactions, hence difficult to reason about and maintain; a design wherein a `sync_io`-pattern core is
 * adapted by an async-I/O-pattern wrapper (as done in the perf-conscious ipc::transport unstructured-IPC impls
 * like `Native_socket_stream`) would likely be much simpler and more elegant.  Most likely this would need to
 * be extended to `Server_session` hierarchy (heavily interacts with `_server_impl`).  (`Client_session` hierarchy
 * would at that point pretty much have to follow at least for consistency.)  This is a major internal refactor --
 * should not be taken lightly -- and incidentally would likely precede any attempt at the to-do elsewhere,
 * where we endeavour to make ipc::session user-extensible.  (A bit of historical background: The internal
 * impl of async-I/O-pattern hierachies in ipc::session preceded the very existence of the `sync_io`-pattern as
 * an idea.  Hence writing `sync_io`-pattern adapters around the already-written async-I/O core -- e.g.,
 * Session_server_adapter -- was more expedient than doing the reverse.  With the benefit of extra hindsight,
 * though, we feel more strongly that (while the status quo is acceptable in and of itself), subsequent maintenance
 * would benefit from the refactor.)  As a bonus/corollary there would be many fewer threads about per session
 * and per server (not a real perf benefit but pretty good).
 *
 * Back to it: Here is how it works, to-dos notwithstanding.
 *
 * For each potential `Server_session` -- i.e., for each async_accept() -- there are 2 steps that must occur
 * asynchronously before one is ready to emit to the user-supplied handler:
 *   - Our stored Native_socket_stream_acceptor, where we invoke
 *     transport::Native_socket_stream_acceptor::async_accept(), must emit a PEER-state (connected)
 *     transport::sync_io::Native_socket_stream (the opposing peer object living inside the opposing Client_session).
 *     - Now a Server_session_mv may be constructed (not yet emitted to user) and then
 *       Server_session_mv::async_accept_log_in() is invoked.
 *     - Or if the socket-accept failed, then we can emit that to the user already; done.
 *   - That Server_session_mv::async_accept_log_in() must complete the async log-in exchange against the
 *     opposing Client_session.
 *     - Now the Server_session_obj can be emitted to the user.
 *     - Or if the log-in fails at some state, then we can emit that to the user.
 *
 * Nomenclature: We call the `N_s_s_a::async_accept()` handler-invoking thread: Wa.  It is officially an
 * unspecified thread or threads, but, by contract, handlers are executed non-concurrently, so it can be
 * considered one thread (and actually it is as of this writing).  We call the
 * Server_session::async_accept_log_in() handler-invoking thread: Ws.  This is really a separate thread for
 * each `Server_session`, so in fact 2 different async-accept requests can cause 2 handlers to invoke simultaneously.
 *
 * So each time one calls async_accept() from thread U (i.e., user thread(s) from which they must never invoke
 * mutating stuff concurrently), that kicks off transport::Native_socket_stream_acceptor::async_accept(),
 * which fires handler in thread Wa; we then kick off `Server_session::async_accept_log_in()`, which fires
 * handler in thread Ws, where we finalize the `Server_session` and emit it to user.  There is no cross-posting
 * to some other worker thread W (but read on).
 *
 * The obvious question is, do the handlers, some of which (as noted) may run concurrently to each other
 * (request 1's Ws handler can co-execute with request 2's Wa handler; and request 2's Wa handler can co-execute
 * with request 3's Wa handler), mess each other over by mutatingly accessing common data?  Let's consider the
 * state involved.
 *
 * For each async-accept request, the amassed data are independent from any other's; they are passed around
 * throughout the 2 async ops per request via lambda captures.  There is, however, one caveat to this:
 * Suppose `S->async_accept_log_in(F)` is invoked on not-yet-ready (incomplete) `Server_session* S`; suppose F is
 * invoked with a truthy #Error_code (it failed).  We are now sitting in thread Ws: and S should be destroyed.
 * But invoking dtor of S from within S's own handler is documented to be not-okay and results in
 * a deadlock/infinite dtor execution, or if the system can detect it, at best an abort due to a thread trying to
 * join itself.  So:
 *   - We maintain State::m_incomplete_sessions storing each such outstanding S.  If dtor runs, then
 *     dtor_stop_accepting() destroys each S's innards, which invokes the user handler with operation-aborted.
 *   - If an incomplete (outstanding) S successfully completes log-in, we remove it from
 *     State::m_incomplete_sessions and emit it to user via handler.
 *   - If it completes log-in with failure, we remove it from State::m_incomplete_sessions and then:
 *     - hand it off to a mostly-idle separate thread, State::m_incomplete_session_graveyard, which can run S's dtor
 *       in peace without deadlocking anyone.  (If `*this` dtor runs before then, the S dtors will still run, as each
 *       queued lambda's captures are destroyed.)
 *       - This is the part that avoids the deadlock.  The other things above are orthogonally needed for the promised
 *         boost.asio-like semantics, where handler must run exactly once eventually, from dtor at the latest.
 *
 * However note that State::m_incomplete_sessions is added-to in thread Wa but removed-from in various threads Ws.
 * Therefore it is protected by a mutex; simple enough.
 *
 * Shutdown is the subtle part, as threads Wa and Ws may be mid-handler when the dtor begins; and those handlers
 * touch our state (and possibly sub-class state via the customization points).  dtor_stop_accepting() joins them
 * all, in a safe order, before anything they touch is destroyed; and the log-in handler recognizes that it is
 * racing it.  This shutdown stuff is indeed tricky and was ultimately the thing that cause me (ygoldfel) to
 * add the to-do above about refactoring Session_server_impl et al around `sync_io`-pattern core(s).
 *
 * @tparam Server_session_t
 *         See #Server_session_obj.  Its API must exactly equal (or be a superset of) that of vanilla #Server_session.
 *         (Its impl may perform extra steps; for example `async_accept_log_in()` might set up a per-session SHM
 *         arena.)
 * @tparam Session_server_t
 *         The class that is in fact `private`ly sub-classing us.
 *         This is necessary for this_session_srv().  See its doc header for discussion.
 */
template<typename Session_server_t, typename Server_session_t>
class Session_server_impl :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// See this_session_srv().
  using Session_server_obj = Session_server_t;

  /// Useful short-hand for the concrete `Server_session` type emitted by async_accept().
  using Server_session_obj = Server_session_t;

  /// Short-hand for Session_mv::Mdt_reader_ptr.
  using Mdt_reader_ptr = typename Server_session_obj::Mdt_reader_ptr;

  /// Short-hand for Session_mv::Channels.
  using Channels = typename Server_session_obj::Channels;

  // Constructors/destructor.

  /**
   * See Session_server ctor; it does that.  In addition:
   *
   * takes and memorizes a functor that takes a Client_app const ref that identifies the app that wants to
   * open the session, performs unspecified synchronous steps, and returns an #Error_code indicating success or
   * reason for failure which dooms that async_accept().
   *
   * ### Rationale for `per_app_setup_func` ###
   * It is not intended for per-session setup.  #Server_session_obj should take care of that where it makes
   * sense -- it does after all represent the individual budding session peer.  However our sub-class
   * (e.g., shm::classic::Session_server) may need to keep track of per-distinct-Client_app resources
   * (e.g., the per-app-scope SHM arena) which must exist before the opposing Client_session-type object
   * completes its setup (e.g., by opening the aforementioned per-Client_app/multi-instance-scope SHM arena).
   * It can detect a new Client_app is logging-in and set that up in the nick of time.
   *
   * @tparam Per_app_setup_func
   *         See above.  Signature: `Error_code F(const Client_app&)`.
   * @param logger_ptr
   *        See Session_server ctor.
   * @param srv_app_ref
   *        See Session_server ctor.
   * @param cli_app_master_set_ref
   *        See Session_server ctor.
   * @param err_code
   *        See Session_server ctor.  Additional #Error_code generated:
   *        see `per_app_setup_func`.
   * @param per_app_setup_func
   *        See above.
   * @param this_session_srv_arg
   *        The object that is, in fact, `private`ly sub-classing `*this` (and calling this ctor).
   *        See this_session_srv().  The value is only saved but not dereferenced inside the ctor.
   */
  template<typename Per_app_setup_func>
  explicit Session_server_impl(flow::log::Logger* logger_ptr,
                               Session_server_obj* this_session_srv_arg,
                               const Server_app& srv_app_ref,
                               const Client_app::Master_set& cli_app_master_set_ref,
                               Error_code* err_code, Per_app_setup_func&& per_app_setup_func);

  /// See Session_server dtor.
  ~Session_server_impl();

  // Methods.

  /**
   * See Session_server method.  In addition: invokes `per_app_setup_func()` (from ctor)
   * once the connecting Client_app becomes known; if that returns truthy #Error_code then this method
   * emits that error.
   *
   * @tparam Task_err
   *         See Session_server method.
   * @tparam N_init_channels_by_srv_req_func
   *         See Session_server method.
   * @tparam Mdt_load_func
   *         See Session_server method.
   * @param target_session
   *        See Session_server method.
   * @param init_channels_by_srv_req
   *        See Session_server method.
   * @param mdt_from_cli_or_null
   *        See Session_server method.
   * @param init_channels_by_cli_req
   *        See Session_server method.
   * @param n_init_channels_by_srv_req_func
   *        See Session_server method.
   * @param mdt_load_func
   *        See Session_server method.
   * @param on_done_func
   *        See Session_server method.
   */
  template<typename Task_err,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept(Server_session_obj* target_session,
                    Channels* init_channels_by_srv_req,
                    Mdt_reader_ptr* mdt_from_cli_or_null,
                    Channels* init_channels_by_cli_req,
                    N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                    Mdt_load_func&& mdt_load_func,
                    Task_err&& on_done_func);

  /**
   * See Session_server method.
   *
   * @return See Session_server method.
   */
  size_t mq_msg_size_limit() const;

  /**
   * See Session_server method.
   *
   * @param limit
   *        See Session_server method.
   */
  void mq_msg_size_limit(size_t limit);

  /**
   * See Session_server method.
   *
   * @param os
   *        See Session_server method.
   */
  void to_ostream(std::ostream* os) const;

  /**
   * Returns pointer to the object that is `private`ly sub-classing us.  In other words this equals
   * `static_cast<const Session_server_obj*>(this)`, where this class is the base of #Session_server_obj,
   * but up-casting from a `private` base is not allowed.
   *
   * ### Rationale ###
   * I (ygoldfel) acknowledge this is somewhat odd.  Why should a sub-class, per se, care or know about its
   * super-class?  This at least vaguely indicates some sort of design mistake.  In fact this is needed, as of
   * this writing, because shm::classic::Server_session_impl::async_accept_log_in() gets a Session_server_impl ptr,
   * which it knows points to an object that's really the core of a shm::classic::Session_server, and it needs to
   * interact with the SHM-classic-specific aspect of that guy's API.  So it calls this accessor here, essentially
   * as a way to up-cast from a `private` base (which is not allowed by C++).
   * Why can't it "just" take a `shm::classic::Session_server*` then?  Answer: because Session_server_impl uses, like,
   * `virtual`-less polymorphism to invoke `async_accept_log_in()` regardless of which object it's calling it on....
   * It's hard to summarize here in words in any way that'll make sense, but if one looks at the relevant code
   * it makes sense.  Eventually.  Bottom line is, this way, it can just pass-in `this`, and then
   * shm::classic::Server_session_impl::async_accept_log_in() can call this_session_srv() to get at the super-class
   * version of `this`.
   *
   * I feel it is not criminal -- internal things are working together in a way that they logically intend to --
   * but intuitively it feels like there's a smoother way to design it.  Probably.
   *
   * @todo Reconsider the details of how classes in the non-`virtual` hierarchies
   * `Session_server`, `Server_session`, `Session_server_impl`, `Server_session_impl` cooperate internally,
   * as there is some funky stuff going on, particularly Session_server_impl::this_session_srv().
   *
   * @return See above.
   */
  Session_server_obj* this_session_srv();

  // Data.

  /// See Session_server public data member.
  const Server_app& m_srv_app_ref;

protected:
  // Methods.

  /**
   * Utility for sub-classes: ensures that `task()` is invoked near the end of `*this` dtor's execution, after *all*
   * other (mutable) state has been destroyed, including stopping/joining any threads performing
   * async_accept() ops.  It may be invoked at most once.
   *
   * The value it adds: A long story best told by specific example.  See the original use case which is
   * in shm::classic::Session_server; it sets up certain SHM cleanup steps to occur,
   * when the session-server is destroyed.
   *
   * ### Watch out! ###
   * At the time `task()` runs, the calling instance of the sub-class has been destroyed -- thus it is, e.g.,
   * usually wrong to capture your `this` in the `task` lambda, except for logging.
   * `get_logger()` and `get_log_component()` (which are in this super-class) are still okay to use.
   *
   * @tparam Task
   *         Function object invoked as `void` with no args.
   * @param task
   *        `task()` shall execute before dtor returns.
   */
  template<typename Task>
  void sub_class_set_deinit_func(Task&& task);

  /**
   * Synchronously stops all async_accept() activity, the post-condition being that no internal handler -- including
   * any invocation of the `per_app_setup_func` customization point, and anything a #Server_session_obj does during
   * its log-in -- is executing or shall execute, and every outstanding async_accept() has fired its handler
   * (with error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER).  Idempotent.
   *
   * Namely it: (1) destroys the socket-stream acceptor, joining thread Wa; (2) fences the log-in handlers off from
   * the incomplete-sessions container, and waits out any handler already past the fence; (3) empties each
   * incomplete session in place, joining its thread Ws, then clears the container; (4) destroys the graveyard thread
   * and anything queued on it.
   *
   * ### The terminal sub-class dtor must call this, first thing ###
   * By the time our dtor runs, the sub-class's own members have already been destroyed; yet async_accept() activity
   * may access sub-class state -- through `per_app_setup_func` (e.g., a per-Client_app arena map and its mutex),
   * or through this_session_srv() from inside a #Server_session_obj log-in.  Hence the terminal sub-class dtor
   * must call dtor_stop_accepting() before its own members are destroyed, i.e., first thing.  (Our dtor asserts
   * this; and calls it anyway, as a release-build fallback that is correct if the sub-class has no such state.)
   */
  void dtor_stop_accepting();

private:
  // Types.

  /**
   * Internally used ref-counted handle to a #Server_session_obj, suitable for capturing and passing around
   * lambdas.
   *
   * ### Rationale ###
   * It is `shared_ptr`, not `unique_ptr`, for two reasons.  Primarily, it is so that it can be captured
   * via #Incomplete_session_observer to avoid a leak that would result from capturing #Incomplete_session
   * in a lambda passed-to an async op on an #Incomplete_session *itself*.  `unique_ptr` cannot be observed
   * via `weak_ptr`; `shared_ptr` can.
   *
   * Secondarily, a `unique_ptr` cannot be captured in a lambda in the first place.
   */
  using Incomplete_session = boost::shared_ptr<Server_session_obj>;

  /**
   * `weak_ptr` observer of an #Incomplete_session.  Capturing this, instead of #Incomplete_session itself,
   * allows for the underlying #Incomplete_session to be destroyed while the lambda still exists.
   */
  using Incomplete_session_observer = boost::weak_ptr<Server_session_obj>;

  /// Short-hand for set of #Incomplete_session, with fast insertion and removal by key #Incomplete_session itself.
  using Incomplete_sessions = boost::unordered_set<Incomplete_session>;

  /// Short-hand for State::m_mutex type.
  using Mutex = flow::util::Mutex_non_recursive;

  /// Short-hand for #Mutex lock.
  using Lock_guard = flow::util::Lock_guard<Mutex>;

  /**
   * All internal mutable state of Session_server_impl.  It's grouped into this inner `struct`, so that we can
   * destroy it in the destructor via an `optional::reset()`, letting items be destroyed automatically in
   * the opposite order in which they were constructed -- and *then* perform certain final steps when
   * that state (including various threads running) is no longer a factor.  See Session_server_impl dtor
   * and sub_class_set_deinit_func().
   */
  struct State
  {
    /* Ordering note: The correctness of shutdown does not rely on the declaration order below: dtor_stop_accepting()
     * explicitly joins every thread that could touch any of this, before our dtor's `m_state.reset()` destroys it.
     * The order is nevertheless chosen defensively per the usual rule -- anything a handler may touch is destroyed
     * after the thread running that handler: #m_master_sock_acceptor (thread Wa) last-declared (destroyed first);
     * then #m_incomplete_sessions (threads Ws); #m_incomplete_session_graveyard and #m_mutex et al earliest-declared
     * (destroyed last). */

    /// The ID used in generating the last Server_session::cli_namespace(); so the next one = this plus 1.  0 initially.
    std::atomic<uint64_t> m_last_cli_namespace;

    /**
     * Protects `m_incomplete_sessions`, `m_stopping`, `m_n_handlers_active`.  See class doc header impl section for
     * discussion of thread design.
     */
    mutable Mutex m_mutex;

    /**
     * `false` until dtor_stop_accepting() fences off the log-in handlers from #m_incomplete_sessions; `true`
     * thereafter.  Written (once) only by dtor_stop_accepting() in thread U, under #m_mutex; read by the log-in
     * handlers (threads Ws) under #m_mutex.  (Thread U may read it without locking, being its only writer.)
     */
    bool m_stopping = false;

    /**
     * The number of log-in handlers (threads Ws) that have passed the #m_stopping check (it being `false`) and
     * erased their session from #m_incomplete_sessions but have not yet finished: they may still post to
     * #m_incomplete_session_graveyard, log via `*this`, and invoke the user's on-done handler.  dtor_stop_accepting()
     * waits for this to reach 0 (after setting #m_stopping, so it cannot grow).  Protected by #m_mutex.
     */
    unsigned int m_n_handlers_active = 0;

    /// Notified whenever #m_n_handlers_active reaches 0.  Used with #m_mutex.
    boost::condition_variable m_handlers_done;

    /**
     * Mostly-idle thread that solely destroys objects removed from `m_incomplete_sessions` in the case where a
     * `async_accept_log_in()` failed as opposed to succeeded (in which case it is emitted to user).
     */
    boost::movelib::unique_ptr<flow::async::Single_thread_task_loop> m_incomplete_session_graveyard;

    /**
     * The set of all #Incomplete_session objects such that each one comes from a distinct async_accept() request
     * that (1) has accepted a transport::sync_io::Native_socket_stream connection and thus created a
     * #Server_session_obj but (2) whose `async_accept_log_in()` has not yet completed (fired
     * handler).
     *
     * Protected by #m_mutex; accessed from thread Wa and threads Ws.  See class doc header impl section for
     * discussion of thread design.
     *
     * ### Impl design ###
     * Once a #Server_session_obj is created by `*this`, we do `async_accept_log_in()` on it.  We cannot capture that
     * `Server_session_obj` itself into its handler's lambda, as that would create a cycle leak wherein
     * the object can never be destroyed.  (This is typical when maintaining a boost.asio-style I/O object by
     * a `shared_ptr` handle.)  So we capture an #Incomplete_session_observer (`weak_ptr`) thereof; but store
     * the `Server_session_obj` (a/k/a #Incomplete_session) here in `*this`, as of course it must be stored somewhere.
     * Then if `*this` dtor is invoked before the aforementioned `async_accept_log_in()` handler fires, the handler
     * shall fire with operation-aborted as desired.
     *
     * See also the ordering caveat at the top of State.
     */
    Incomplete_sessions m_incomplete_sessions;

    /// transport::Native_socket_stream acceptor avail throughout `*this` to accept init please-open-session requests.
    boost::movelib::unique_ptr<transport::Native_socket_stream_acceptor> m_master_sock_acceptor;
  }; // struct State

  // Data.

  /**
   * See mq_msg_size_limit(); starts at zero but can be overridden to another value and/or back to zero via
   * mutator.  Saved as an alignment-friendly multiple.
   */
  size_t m_mq_msg_size_limit;

  /// See this_session_srv().
  Session_server_obj* const m_this_session_srv;

  /// See ctor.
  const Client_app::Master_set& m_cli_app_master_set_ref;

  /// See ctor.
  const Function<Error_code (const Client_app& client_app)> m_per_app_setup_func;

  /// See State.
  std::optional<State> m_state;

  /// See sub_class_set_deinit_func().  `.empty()` unless that was called at least once.
  Function<void ()> m_deinit_func_or_empty;
}; // class Session_server_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SESSION_SERVER_IMPL \
  template<typename Session_server_t, typename Server_session_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SESSION_SERVER_IMPL \
  Session_server_impl<Session_server_t, Server_session_t>

TEMPLATE_SESSION_SERVER_IMPL
template<typename Per_app_setup_func>
CLASS_SESSION_SERVER_IMPL::Session_server_impl
  (flow::log::Logger* logger_ptr, Session_server_obj* this_session_srv_arg, const Server_app& srv_app_ref_arg,
   const Client_app::Master_set& cli_app_master_set_ref, Error_code* err_code,
   Per_app_setup_func&& per_app_setup_func) :

  flow::log::Log_context(logger_ptr, Log_component::S_SESSION),
  m_srv_app_ref(srv_app_ref_arg), // Not copied!
  m_mq_msg_size_limit(0), // Default = let us choose it.
  m_this_session_srv(this_session_srv_arg),
  m_cli_app_master_set_ref(cli_app_master_set_ref), // Ditto!
  m_per_app_setup_func(std::move(per_app_setup_func)),
  m_state(std::in_place) // Default-ct State; initialize contents, further, just below.
{
  using util::Process_credentials;
  using transport::sync_io::Native_socket_stream;
  using flow::error::Runtime_error;
  using flow::async::reset_this_thread_pinning;
  using flow::log::Sev;
  using boost::movelib::make_unique;
  using boost::system::system_category;
  using boost::io::ios_all_saver;
  using std::string;
  using fs::ofstream;
  using Named_sh_mutex = boost::interprocess::named_mutex;
  using Named_sh_mutex_ptr = boost::movelib::unique_ptr<Named_sh_mutex>;
  using Sh_lock_guard = boost::interprocess::scoped_lock<Named_sh_mutex>;
  // using ::errno; // It's a macro apparently.

  Error_code our_err_code;
  Server_session_obj empty_session_public;
  const typename Server_session_dtl<Server_session_obj>::Session_base_obj* empty_session_base = {};

  /* Validate the App names per the contract (see App::m_name doc header): they get embedded into
   * `Shared_name`s per the conventional naming scheme, and a name containing S_SEPARATOR corrupts that
   * scheme -- often silently, since much consuming logic is prefix-based and tolerates it, while
   * decompose-based logic (e.g., the SHM-jemalloc crash-cleanup sweeps) quietly misbehaves.  So fail
   * loudly instead, right here, where the entire universe of App names for this Flow-IPC split is in hand. */
  {
    const auto name_bad = [](const string& name) -> bool
    {
      return name.empty() || (name.find(Shared_name::S_SEPARATOR) != string::npos);
    };
    // Attn: An empty name *is* one of the bad-name forms; so "" cannot double as a none-found sentinel.
    bool found_bad_name = name_bad(m_srv_app_ref.m_name);
    string bad_name = found_bad_name ? m_srv_app_ref.m_name : string{};
    if (!found_bad_name)
    {
      for (const auto& cli_app_name_and_obj : cli_app_master_set_ref)
      {
        if (name_bad(cli_app_name_and_obj.first))
        {
          found_bad_name = true;
          bad_name = cli_app_name_and_obj.first;
          break;
        }
      }
    }
    if (found_bad_name)
    {
      FLOW_LOG_WARNING("Session-server [" << *this << "]: An App name -- [" << bad_name << "] -- is "
                       "empty or contains the Shared_name separator [" << Shared_name::S_SEPARATOR << "]; "
                       "this violates the App::m_name contract and would corrupt the conventional "
                       "shared-resource naming scheme.  Emitting error.");
      our_err_code = error::Code::S_INVALID_ARGUMENT;
    }
  }

  if (!our_err_code)
  {
    // Finish setting up m_state.  See State members in order and deal with the ones needing explicit init.
    m_state->m_last_cli_namespace = 0;
    m_state->m_incomplete_session_graveyard
      = boost::movelib::make_unique<flow::async::Single_thread_task_loop>
          (get_logger(),
           /* (Linux) OS thread name will truncate the this-addr snippet to 15-5=10 chars here;
            * which should actually just fit.  Nothing else seems particularly useful;
            * like in non-exotic setups our srv-name is pretty much known. */
           flow::util::ostream_op_string("SSvG-", this));

    /* This is a (as of this writing -- the) *cleanup point* for any MQs previously created on behalf of this
     * Server_app by previous active processes before us; namely when either a Server_session or opposing Client_session
     * performs open_channel() (or pre-opens channel(s) during session creation), so the Server_session_impl
     * creates the actual `Persistent_mq_handle`s via its ctor in create-only mode.  These underlying MQs
     * are gracefully cleaned up in Blob_stream_mq_sender/receiver dtors (see their doc headers).  This cleanup point
     * is a best-effort attempt to clean up anything that was skipped due to one or more such destructors never
     * getting to run (due to crash, abort, etc.).  Note that Blob_stream_mq_sender/receiver doc headers explicitly
     * explain the need to worry about this contingency.
     *
     * We simply delete everything with the Shared_name prefix used when setting up the MQs
     * (see Server_session_impl::make_channel_mqs()).  The prefix is everything up-to (not including) the PID
     * (empty_session_base->srv_namespace() below).  Our own .srv_namespace() is
     * just about to be determined and is unique across time by definition (internally, it's -- again -- our PID);
     * so any existing MQs are by definition old.  Note that as of this writing there is at most *one* active
     * process (instance) of a given Server_app.
     *
     * Subtlety (kind of): We worry about such cleanup only if some type of MQ is in fact enabled at compile-time
     * of *this* application; and we only clean up that MQ type, not the other(s) (as of this writing there are 2,
     * but that could change conceivably).  If the MQ type is changed (or MQs disabled) after a crash/abort, there
     * could be a leak.  We could also indiscriminately clean-up all known MQ types here; that would be fine.
     * @todo Maybe we should.  I don't know.  shm::classic::Session_server ctor's pool cleanup point is only invoked,
     * if that is the type of Session_server user chose at compile-time, so we are just following that example.
     * Doing it this way strikes me as cleaner code, and the combination of a crash/abort and changed software
     * "feels" fairly minor.
     *
     * A note on stats: Stats support for this cleanup point (and its analogs in the SHM-enabled `Session_server`
     * variants) has been considered and deliberately omitted: such events are rare by construction (crash
     * aftermath) and fully log-observable (per-item logging and counts inside `remove_each_persistent_*()`).
     * Longer discussion: see similar note in shm::arena_lend::jemalloc::Session_server::cleanup(). */
    if constexpr(Server_session_obj::S_MQS_ENABLED)
    {
      using transport::Blob_stream_mq_base;
      using Mq = typename Server_session_dtl<Server_session_obj>::Session_base_obj::Persistent_mq_handle_from_cfg;

      util::remove_each_persistent_with_name_prefix<Blob_stream_mq_base<Mq>>
        (get_logger(),
         build_conventional_shared_name_prefix(Mq::S_RESOURCE_TYPE_ID,
                                               Shared_name::ct(m_srv_app_ref.m_name)));
    }

    /* We want to write to CNS (PID file) just below, but what is its location, and the name of its associated
     * inter-process mutex, and for that matter the contents (formally, the Current Namespace; really the PID)?
     * Well, this is slightly cheesy, arguably, but any Server_session we produce will need all those values,
     * and can compute them by itself, and they'll always be the same in this process (app instance), so let's
     * just make this short-lived dummy Server_session_obj and get the stuff out of there.  Code reuse = pretty good. */
    empty_session_public
      = Server_session_dtl<Server_session_obj>::ct_base(nullptr, m_srv_app_ref, Native_socket_stream{});
    empty_session_base // Use privileged-API wrapper to get at protected .base().
      = &(Server_session_dtl<Server_session_obj>{ empty_session_public }.base());

    /* Owner/mode discussion:
     * ipc::session operates in a certain model (design doc is elsewhere/outside our scope here to fully justify)
     * that requires, for security/safety:
     *   - Owner has the specific UID:GID registered under Server_app.  If we are who we are supposed to be,
     *     this will occur automatically as we create a resource.  Namely we have two relevant resources in here:
     *     - CNS (PID file).  Some reasons this could fail: if file already existed
     *       *and* was created by someone else; if we have the proper UID but are also in some other group or something;
     *       and lastly Server_app misconfiguration.  Mitigation: none.  For one of those we could do an owner-change
     *       call to change the group, but for now let's say it's overkill, and actually doing so might hide
     *       a problem in the environment: let's not even risk that stuff.
     *     - Associated shared mutex (in Linux apparently a semaphore thingie).  This is an interesting situation;
     *       it is not mentioned in the aforementioned design in detail -- too much of an impl detail for that --
     *       so let's consider it.  Should the Server_app UID:GID apply to it too?  Actually not quite: the way
     *       we use it, it's a symmetrically shared resource (read/write for both us and client), but more to the point
     *       the *client* is *allowed* to create it (hence OPEN_OR_CREATE both here and in Client_session_impl): it's
     *       accessed in order to get one's turn at accessing the CNS file, and to be "accessed" it must be created
     *       as needed (and then it'll keep existing until reboot).  So actually the *GID* should be correct
     *       according to Server_app, but the UID can be, in fact, Server_app's -- or Client_app's, or *another*
     *       Client_app's entirely, or any number of them!
     *       - So should we check something about the mutex's UID/GID then?
     *         - It would probably be not bad to check for the GID.  It is even conceivable to check the UID as that
     *           of one of the allowed `Client_app`s.
     *         - I (ygoldfel) feel it's overkill.  I could be wrong, but it just feels insane: the mutex is only a way
     *           to access CNS file in orderly fashion without concurrency issues.  Either it works, or it doesn't
     *           work; trying to sanity-check that the right UID/GID owns it is going beyond the spirit of the design:
     *           to ascertain that "certain model" of trust/safety/security.  We already do that with CNS itself;
     *           we don't need to be paranoid about the-thing-that-is-needed-to-use-CNS.
     *   - The mode is as dictated by Server_app::m_permissions_level_for_client_apps.  This we can and should
     *     ensure via a mode-set/change call.  There are subtleties about how to do that, but they're discussed
     *     near the call sites below.  As for now: We have two relevant resources in here, again:
     *     - CNS (PID file).  Yes, indeed, we set its permissions below.
     *     - Associated shared mutex.  See above.  So, actually, we set its permissions below too.  That is actually
     *       (though unmentioned in the design) a pretty good idea for the "certain model" in the design:
     *       If, say, it's set to unrestricted access, then any user could just acquire the lock... and block IPC
     *       from proceeding, ever, for anyone else wishing to work with that Server_app.  So, certainly,
     *       it should be locked down to the same extent as CNS itself.  What's interesting about that is that
     *       a client, too, can create it (again, see above) -- and thus needs to set some kind of sensible
     *       permissions on creation (if applicable).  And so it does... by checking Server_app [sic] as well and
     *       indeed setting that level of permissions.  Does it make sense?  Yes.  Here are the scenarios specifically:
     *       - If the access level in Server_app is set to NO_ACCESS: Well then no one can access it.  LoL.  Next.
     *       - If UNRESTRICTED: Well then it's meant to be world-accessible!  Yay!  LoL again!  Next.
     *       - If USER_ACCESS: Logically, for CNS to be accessible, if only the server UID is allowed access, then
     *         for it to interoperate with any clients, the clients must also be of that UID.  So the client
     *         setting the mode to *its* UID should accomplish the same as if server had done it.
     *       - If GROUP_ACCESS (the approach mandated by the design, though we don't assume it, hence the
     *         Server_app configuration): Same logic but applied to GID.
     *       Anyway, here on the server it's simple; we should set-mode similarly to how we do for CNS.
     *       The bulk of the above long explanation is why the client does it.  The comment in Client_session_impl
     *       points back to right here to avoid duplication.
     *
     * So let's do that stuff below. */

    /* Ensure our effective user is as configured in Server_app.  We do check this value on the CNS (PID file)
     * anyway; but this is still a good check because:
     *   - We might not be creating the file ourselves (first guy to get to it since boot is).
     *   - It eliminates the need (well, strong word but anyway) to keep checking that for various other
     *     resources created down the line, whether it's MQs or SHM pools or whatever else.
     *     Doing that is possible but:
     *     - annoying: it requires either a descriptor for an ::fstat() or a rather-unportable file-system-path for
     *       anything but actual files;
     *     - super-annoying: it requires deep-inside -- possibly even eventually user-supplied -- modules like
     *       the SHM-jemalloc module to be aware of the desire to even do this in the first place on every shared
     *       resource.
     *
     * The idea is: checking it here up-front, plus checking it on the CNS (PID file) (from which all IPC naming
     * and thus trust, per design, flows), is a nice way to take care of it ahead of all that.  It's not perfect:
     * the effective UID:GID can be changed at runtime.  We don't need to be perfect though: the whole
     * safety/security project, here, is not meant to be some kind of cryptographically powerful guarantee. */
    const auto own_creds = Process_credentials::own_process_credentials();
    if ((own_creds.user_id() != m_srv_app_ref.m_user_id) || (own_creds.group_id() != m_srv_app_ref.m_group_id))
    {
      FLOW_LOG_WARNING("Session acceptor [" << *this << "]: Creation underway.  However, just before writing "
                       "CNS (Current Namespace Store), a/k/a PID file, we determined that "
                       "the `user` aspect of our effective credentials [" << own_creds << "] does not match "
                       "the hard-configured value passed to this ctor: "
                       "[" << m_srv_app_ref.m_user_id << ':' << m_srv_app_ref.m_group_id << "].  "
                       "We cannot proceed, as this would violate the security/safety model of ipc::session.  "
                       "Emitting error.");
      our_err_code = error::Code::S_RESOURCE_OWNER_UNEXPECTED;
    }
    /* Similarly ensure we were invoked as the executable Server_app says.  Every opposing Client_session checks
     * exactly this about us (and refuses the session if it fails); so better to fail here, early and clearly, than
     * to have every session-open fail on the client side. */
    else if (const auto own_invoked_as = own_creds.process_invoked_as(&our_err_code);
             our_err_code || (own_invoked_as != m_srv_app_ref.m_exec_path.string()))
    {
      if (our_err_code)
      {
        FLOW_LOG_WARNING("Session acceptor [" << *this << "]: Creation underway.  However, just before writing "
                         "CNS (Current Namespace Store), a/k/a PID file, we failed to query our own "
                         "executable-invoked-as (binary name) value "
                         "([" << our_err_code << "] [" << our_err_code.message() << "]).  Emitting error.");
      }
      else
      {
        FLOW_LOG_WARNING("Session acceptor [" << *this << "]: Creation underway.  However, just before writing "
                         "CNS (Current Namespace Store), a/k/a PID file, we determined that we were invoked as "
                         "[" << own_invoked_as << "], which does not *exactly* match the hard-configured "
                         "App::m_exec_path passed to this ctor: [" << m_srv_app_ref.m_exec_path.string() << "].  "
                         "Every opposing Client_session would refuse to open a session with us.  Emitting error.");
        our_err_code = error::Code::S_SERVER_APP_EXEC_PATH_INCONSISTENT;
      }
    }
    else // if (own_creds match m_srv_app_ref.m_{user|group}_id, and invoked-as matches m_exec_path)
    {
      const auto mutex_name = empty_session_base->cur_ns_store_mutex_absolute_name();
      const auto mutex_perms = util::shared_resource_permissions(m_srv_app_ref.m_permissions_level_for_client_apps);
      const auto cns_path = empty_session_base->cur_ns_store_absolute_path();
      const auto cns_perms = util::PRODUCER_CONSUMER_RESOURCE_PERMISSIONS_LVL_MAP
                               [size_t(m_srv_app_ref.m_permissions_level_for_client_apps)];

      const auto logger_ptr = get_logger();
      if (logger_ptr && logger_ptr->should_log(Sev::S_INFO, get_log_component()))
      {
        ios_all_saver saver{*(logger_ptr->this_thread_ostream())}; // Revert std::oct/etc. soon.
        FLOW_LOG_INFO_WITHOUT_CHECKING
          ("Session acceptor [" << *this << "]: Created.  Writing CNS (Current Namespace Store), a/k/a PID "
           "file [" << cns_path << "] (perms "
           "[" << std::setfill('0')
               << std::setw(4) // Subtlety: This resets right away after the perms are output...
               << std::oct << cns_perms.get_permissions() << "], "
           "shared-mutex name [" << mutex_name << "], shared-mutex perms "
           "[" << std::setw(4) // ...hence gotta do this again.
               << mutex_perms.get_permissions() << "]); "
           "then listening for incoming master socket stream "
           "connects (through Native_socket_stream_acceptor that was just cted) to address "
           "based partially on the namespace (PID) written to that file.");
      }

      /* See Client_session_impl where it, too, creates this sh_mutex for notes equally applicable here.
       * It reads the file we are about to write and locks the same inter-process mutex accordingly. */
      Named_sh_mutex_ptr sh_mutex;
      util::op_with_possible_bipc_exception(get_logger(), &our_err_code, error::Code::S_MUTEX_BIPC_MISC_LIBRARY_ERROR,
                                            "Session_server_impl::ctor:named-mutex-open-or-create", [&]()
      {
        sh_mutex = make_unique<Named_sh_mutex>(util::OPEN_OR_CREATE, mutex_name.native_str(), mutex_perms);
        /* Set the permissions as discussed in long comment above. --^
         * Bonus: All bipc OPEN_OR_CREATE guys take care to ensure permissions are set regardless of umask,
         * so no need for us to set_resource_permissions() here.
         *
         * As for ensuring ownership... skipping as discussed in long comment above. */
      });

      if (!our_err_code)
      {
        Sh_lock_guard sh_lock{*sh_mutex};

        /* Only set permissions if we in fact create CNS (PID file).  Since we use mutex and trust mutex's other
         * users, we can atomically-enough check whether we create it by pre-checking its existence.  To pre-check
         * its existence use fs::exists().  fs::exists() can yield an error, but we intentionally eat any error
         * and treat it as-if file does not exist.  Whatever issue it was, if any, should get detected via ofstream
         * opening.  Hence, if there's an error, we pre-assume we_created_cns==true, and let the chips fall where
         * they may subsequently.  (Note this is all a low-probability eventuality.) */
        Error_code dummy; // Can't just use fs::exists(cns_path), as it might throw an exception (not what we want).
        const bool we_created_cns = !fs::exists(cns_path, dummy);
        ofstream cns_file{cns_path};
        // Make it exist immediately (for the following check).  @todo Might be unnecessary.  At least it's harmless.
        cns_file.flush();

        /* Ensure owner is as configured.  (Is this redundant, given that we checked UID:GID above?  Yes and no:
         * yes, if we created it; no, if we hadn't.  So why not only check if (!we_created_cns)?  Answer: paranoia,
         * sanity checking.  We're not gonna do this check down the line for this session (per earlier-explained
         * decision), so might as well just sanity-check.
         *
         * Ideally we'd:
         *   - Pass in an fstream-or-FD-or-similar (we've opened the file after all, so there is one), not a name.
         *   - Use something in C++/C standard library or Boost, not an OS call.
         *
         * It's nice to want things.  On the FD front we're somewhat screwed; there is a gcc-oriented hack to get
         * the FD, but it involves protected access and non-standard stuff.  Hence we must work from the `path`
         * cns_path.  We've created it, so it really should work, even if it's a little slower or what-not.
         * As for using a nice library... pass the buck: */
        ensure_resource_owner_is_app(get_logger(), cns_path, m_srv_app_ref, &our_err_code);
        if ((!our_err_code) && we_created_cns)
        {
          /* The owner check passed; and we just created it.  (At this stage fs::exists() having failed somehow is
           * more-or-less impossible: ensure_resource_owner_is_app() would've failed if so.)  So:
           * Set mode.  Again, no great way to get an FD, nor to use the fstream itself.  So just: */
          util::set_resource_permissions(get_logger(), cns_path, cns_perms, &our_err_code);
          // If it failed, it logged.
        } // if (we_created_cns && (!(our_err_code [from set_resource_permissions()])))

        if (!our_err_code)
        {
          cns_file << empty_session_base->srv_namespace().str() << '\n';

          if (!cns_file.good())
          {
            const auto sys_err_code = our_err_code = {errno, system_category()};
            FLOW_LOG_WARNING("Session acceptor [" << *this << "]: Could not open or write CNS (PID) "
                             "file [" << cns_path << "]; system error details follow.");
            FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
          }
          // Close file, unlock mutex.
        } // if (!our_err_code) (from ensure_resource_owner_is_app(), or from set_resource_permissions())
        // else { It logged. }
      } // if (!our_err_code) (mutex creation)
      // else { It logged. }

      sh_mutex.reset(); // Again, see note in Client_session_impl which does the same.
    } // else if (own_creds match m_srv_app_ref.m_{user|group}_id) // Otherwise our_err_code is truthy.
  } // if (!our_err_code) (App-name check)

  if (our_err_code)
  {
    if (err_code)
    {
      *err_code = our_err_code;
      return;
    }
    // else
    throw Runtime_error{our_err_code, FLOW_UTIL_WHERE_AM_I_STR()};
  }
  /* Got here: CNS (PID file) written fine.  Now we can listen on the socket stream acceptor derived off that value:
   * clients will read that file and thus know to connect to that. */

  // This might throw; or emit failure to *err_code; or neither (success).
  m_state->m_master_sock_acceptor
    = make_unique<transport::Native_socket_stream_acceptor>
        (get_logger(),
         empty_session_base->session_master_socket_stream_acceptor_absolute_name(),
         err_code);

  // See class doc header.  Start this very-idle thread for a bit of corner case work.
  if ((!err_code) || (!*err_code)) // Attn: null err_code = throwing form: if we are here, it succeeded.
  {
    m_state->m_incomplete_session_graveyard->start(reset_this_thread_pinning);
    // Don't inherit any strange core-affinity!  ^-- Worker must float free.
  }
  // else { Might as well not start that thread. }
} // Session_server_impl::Session_server_impl()

TEMPLATE_SESSION_SERVER_IMPL
CLASS_SESSION_SERVER_IMPL::~Session_server_impl()
{
  /* All async_accept() activity must be stopped already, by the terminal sub-class dtor (see dtor_stop_accepting()
   * doc header); so no internal handler executes, and the rest of the state is idle and can be destroyed in any
   * order.  If a sub-class forgot, we do it now (too late if in-flight activity reaches sub-class state), and
   * complain in debug builds. */
  assert(m_state->m_stopping && "Terminal Session_server sub-class dtor must call dtor_stop_accepting() first.");
  dtor_stop_accepting();

  /* Additionally via sub_class_set_deinit_func() we allow for certain final de-init code to be executed, once
   * all state has been destroyed.  Hence force this to occur now: */
  m_state.reset();

  // Last thing!  See sub_class_set_deinit_func() doc header which will eventually lead you to a rationale comment.
  if (!m_deinit_func_or_empty.empty())
  {
    FLOW_LOG_INFO("Session acceptor [" << *this << "]: Continuing shutdown.  A sub-class desires final de-init "
                  "work, once everything else is stopped (might be persistent resource cleanup); invoking "
                  "synchronously.");
    m_deinit_func_or_empty();
    FLOW_LOG_TRACE("De-init work finished.");
  }
} // Session_server_impl::~Session_server_impl()

TEMPLATE_SESSION_SERVER_IMPL
void CLASS_SESSION_SERVER_IMPL::dtor_stop_accepting()
{
  // We are in thread U (from a sub-class dtor, or from our own dtor).

  if (m_state->m_stopping) // (We are its only writer; so no need to lock m_mutex to read it.)
  {
    return; // Already done.
  }
  // else

  FLOW_LOG_INFO("Session acceptor [" << *this << "]: Shutting down.  The Native_socket_stream_acceptor will now "
                "shut down, and all outstanding Native_socket_stream_acceptor handlers and Server_session "
                "handlers shall fire with operation-aborted error codes.");

  /* (1) Destroy the acceptor: joins thread Wa.  Any pending acceptor async-accept fires our handler with its
   * operation-aborted code; we translate it into the expected operation-aborted code.  A handler already executing
   * (successful socket-accept) completes first, possibly adding one more incomplete session; does not matter.
   * (Null if ctor failed early; then nothing to do; and no async_accept() can have been called anyway.) */
  m_state->m_master_sock_acceptor.reset();

  /* (2) Fence off the log-in handlers (threads Ws) from m_incomplete_sessions.  Each handler checks m_stopping under
   * m_mutex before touching the container.  So: any handler that did touch it did so before this point; any later
   * one sees m_stopping and leaves it alone.  From here on we (thread U) are its only accessor; so no locking is
   * needed below.  (Thread Wa, the only other accessor, is gone per (1).)
   *
   * Also wait out any handler that got in before the fence: it erased its session from m_incomplete_sessions --
   * so (3) will not join its thread Ws -- but it may still be posting to the graveyard, logging via *this, and
   * invoking the user's handler.  It cannot be allowed to do so after (4), let alone after we return.  (No new
   * such handler can appear: m_stopping is now set.) */
  {
    Lock_guard incomplete_sessions_lock{m_state->m_mutex};
    m_state->m_stopping = true;
    while (m_state->m_n_handlers_active != 0)
    {
      m_state->m_handlers_done.wait(incomplete_sessions_lock);
    }
  }

  /* (3) Empty each incomplete session in place: the move-assignment destroys its impl, whose dtor joins its
   * thread Ws, first firing any pending async_accept_log_in() handler (ours) with operation-aborted.  That handler
   * sees m_stopping and simply reports operation-aborted to the user (see it in async_accept()).  Note that the
   * container keeps holding each session's `shared_ptr` throughout; hence the handler's own ref (from
   * `weak_ptr::lock()`) is never the last one -- so it never destroys its session from its own thread; and it
   * needs no graveyard. */
  for (const auto& incomplete_session : m_state->m_incomplete_sessions)
  {
    *incomplete_session = Server_session_obj{};
  }
  // All have been replaced with inert NULL-state husks: no threads Ws left.

  /* (4) Destroy the graveyard: joins its thread; anything still queued on it (sessions whose log-in failed, handed
   * off by the handler before the end of (2)) is destroyed here, in thread U.  No thread Ws exists anymore to post
   * onto it.  (Null if ctor failed early.) */
  m_state->m_incomplete_session_graveyard.reset();
} // Session_server_impl::dtor_stop_accepting()

TEMPLATE_SESSION_SERVER_IMPL
template<typename Task_err,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_SESSION_SERVER_IMPL::async_accept(Server_session_obj* target_session,
                                             Channels* init_channels_by_srv_req,
                                             Mdt_reader_ptr* mdt_from_cli_or_null,
                                             Channels* init_channels_by_cli_req,
                                             N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                                             Mdt_load_func&& mdt_load_func,
                                             Task_err&& on_done_handler)
{
  using util::String_view;
  using flow::async::Task_asio_err;
  using boost::make_shared;
  using std::to_string;
  using std::string;

  assert(target_session);
  assert(m_state->m_master_sock_acceptor && "By contract do not invoke async_accept() if ctor failed.");

  /* We are in thread U or Wa or one of the Ws.  U is obviously fine/mainstream.  Ws means they invoked us
   * directly from our own completion handler again; it is fine, since a Ws is started per async_accept();
   * does not interact with other Ws.  Wa means they invoked us directly from our own completion handler
   * again, albeit only on error would that be from Wa; anyway Native_socket_stream_acceptor allows it. */

  Task_asio_err on_done_func{std::move(on_done_handler)};
  auto sock_stm = make_shared<transport::sync_io::Native_socket_stream>(); // Empty target socket stream.
  const auto sock_stm_raw = sock_stm.get();

  FLOW_LOG_INFO("Session acceptor [" << *this << "]: Async-accept request: Immediately issuing socket stream "
                "acceptor async-accept as step 1.  If that async-succeeds, we will complete the login asynchronously; "
                "if that succeeds we will finally emit the ready-to-go session to user via handler.");
  m_state->m_master_sock_acceptor->async_accept
    (sock_stm_raw,
     [this, target_session, init_channels_by_srv_req, mdt_from_cli_or_null,
      init_channels_by_cli_req,
      n_init_channels_by_srv_req_func = std::move(n_init_channels_by_srv_req_func),
      mdt_load_func = std::move(mdt_load_func),
      sock_stm = std::move(sock_stm),
      on_done_func = std::move(on_done_func)]
       (const Error_code& err_code) mutable
  {
    // We are in thread Wa (unspecified; really N_s_s_acceptor worker thread).

    if (err_code)
    {
      /* Couple cases to consider.  1, could've failed with a true (probably system) error.
       * 2, it could've failed with operation-aborted which can only occur if m_master_sock_acceptor is destroyed
       * during the async_accept(), which in turn can only happen if *this is destroyed during it.
       *
       * 1 is straightforward; just report the error through user handler, and that's that.  As for 2:
       *
       * Since we've decided to (as explained in Implementation section of our class doc header) fully piggy-back
       * all work, including handlers, on our helper objects' (m_master_sock_acceptor, or in step 2 the
       * Server_session itself) worker threads, we can just report operation-aborted through user handler as well.
       * Our dtor doesn't then need to worry about (having tracked un-fired user handlers) firing un-fired handlers:
       * we will just do so by piggy-backing on m_master_sock_acceptor doing so.  Code is much simpler then.
       *
       * Subtlety: avoid even logging.  Logging is safe for 1, but it's not that high in value.  I believe
       * logging is safe in 2, because m_master_sock_acceptor dtor will only return once it has fired the
       * handler (with operation-aborted), which happens before Log_context super-class is destroyed... but
       * generally we avoid extra code on operation-aborted everywhere in Flow-IPC and `flow` (that I (ygoldfel)
       * control anyway), so let's just keep to that.  @todo Maybe re-add TRACE logging when not operation-aborted.
       *
       * If you change this, please consider synchronizing with the async_accept_log_in() handler below.
       *
       * Subtlety: transport:: and session:: have separate operation-aborted codes, and we promised the latter so: */
      on_done_func((err_code == transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
                     ? error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER : err_code);
      return;
    } // if (err_code)
    // else: No problem at this async-stage.

    FLOW_LOG_INFO("Session acceptor [" << *this << "]: Async-accept request: Socket stream "
                  "acceptor async-accept succeeded resulting in socket stream [" << *sock_stm << "]; that was step "
                  "1.  Now we create the server session and have it undergo async-login; if that succeeds we will "
                  "finally emit the ready-to-go session to user via handler.");

    Incomplete_session incomplete_session // Use privileged-API wrapper to construct Server_session_obj via priv ctor.
      = make_shared<Server_session_obj>(Server_session_dtl<Server_session_obj>::ct_base
                                          (get_logger(),
                                           m_srv_app_ref, // Not copied!
                                           // Gobble up the stream obj into the channel in session:
                                           std::move(*sock_stm)));
    /* It's important that we save this in *this, so that if *this is destroyed, then *incomplete_session is
     * destroyed, so that it triggers the handler below with operation-aborted, so that we pass that on to
     * the user handler as promised.  However it is therefore essential that we don't also capture
     * incomplete_session (the shared_ptr!) in our handler lambda but rather only the observer (weak_ptr) of it!
     * Otherwise we create a cycle and a memory leak. */
    {
      Lock_guard incomplete_sessions_lock{m_state->m_mutex};
      m_state->m_incomplete_sessions.insert(incomplete_session);
    }

    // This little hook is required by Server_session_mv::async_accept_log_in() for Client_app* lookup and related.
    auto cli_app_lookup_func = [this](String_view cli_app_name) -> const Client_app*
    {
      const auto cli_it = m_cli_app_master_set_ref.find(string{cli_app_name});
      return (cli_it == m_cli_app_master_set_ref.end()) ? nullptr : &cli_it->second;
    };

    /* And this one is required to issue a unique cli-namespace, if all goes well.  The "if all goes well" part
     * is why it's a hook and not just pre-computed and passed-into async_accept_log_in() as an arg. */
    auto cli_namespace_func = [this]() -> Shared_name
    {
      return Shared_name::ct(to_string(++m_state->m_last_cli_namespace));
    };

    /* Lastly, we are to execute this upon knowing the Client_app and before the log-in response it sent to
     * opposing Client_session. */
    auto pre_rsp_setup_func = [this,
                               incomplete_session_observer = Incomplete_session_observer{incomplete_session}]
                                () -> Error_code
    {
      // We are in thread Ws (unspecified; really Server_session worker thread).

      // (Could have captured incomplete_session itself, but I'd rather assert() than leak.)
      auto incomplete_session = incomplete_session_observer.lock();
      assert(incomplete_session
             && "The Server_session_obj cannot be dead (dtor ran), if it's invoking its async handlers OK "
                  "(and thus calling us in its async_accept_log_in() success path.");
      // If it's not dead, we're not dead either (as us dying is the only reason incomplete_session would die).

      const auto cli_app_ptr = Server_session_dtl<Server_session_obj>{ *incomplete_session }.base().cli_app_ptr();
      assert(cli_app_ptr && "async_accept_log_in() contract is to call pre_rsp_setup_func() once all "
                              "the basic elements of Session_base are known (including Client_app&).");

      return m_per_app_setup_func(*cli_app_ptr);
    }; // auto pre_rsp_setup_func =

    /* @todo It occurs to me (ygoldfel) now that I look at it: all these args -- even on_done_func --
     * could instead be passed into Server_session_obj ctor; that guy could save them into m_* (as of this writing
     * it passes them around via lambdas).  After all, as of now, async_accept_log_in() is non-retriable.
     * Pros: much less lambda-capture boiler-plate in there.
     * Cons: (1) added state can leak (they'd need to worry about maybe clearing that stuff on entry to almost-PEER
     * state, and possibly on failure); (2) arguably less maintainable (maybe Server_session_obj may want to
     * make async_accept() re-triable a-la Client_session_impl::async_connect()?).
     *
     * I, personally, have a big bias against non-const m_* state (where it can be avoided reasonably), so I
     * like that this stuff only stays around via captures, until the relevant async-op is finished.  So I prefer
     * it this way.  Still: arguable.
     *
     * Anyway.  Go! */

    Server_session_dtl<Server_session_obj>{ *incomplete_session }.async_accept_log_in
      (this,
       init_channels_by_srv_req, // Our async out-arg for them to set on success (before on_done_func(Error_code{})).
       mdt_from_cli_or_null, // Ditto.
       init_channels_by_cli_req, // Ditto.

       std::move(cli_app_lookup_func), // Look up Client_app by name and give it to them!
       std::move(cli_namespace_func), // Generate new per-session namespace and give it to them!
       std::move(pre_rsp_setup_func), // Set up stuff necessary for this Client_app, especially if 1st occurrence!

       std::move(n_init_channels_by_srv_req_func), // How many channels does our caller want to set up?
       std::move(mdt_load_func), // Let caller fill out srv->cli metadata!

       [this, incomplete_session_observer = Incomplete_session_observer{incomplete_session},
        target_session, on_done_func = std::move(on_done_func)]
         (const Error_code& async_err_code)
    {
      // We are in thread Ws (unspecified; really Server_session worker thread).

      /* The Server_session_obj is alive: m_incomplete_sessions holds it until we erase it just below -- including
       * throughout dtor_stop_accepting() (which empties it in place, keeping the `shared_ptr`, before clearing). */
      auto incomplete_session = incomplete_session_observer.lock();
      assert(incomplete_session && "m_incomplete_sessions holds it until this handler, or shutdown, is done.");

      /* Are we shutting down (dtor_stop_accepting() underway)?  If so, do not touch m_incomplete_sessions (it is
       * being operated on by thread U, sans locking).  Else the session is no longer incomplete; either it accepted
       * log-in OK or not.  Remove it from *this.  We'll either forget it (error) or give it to user (otherwise). */
      bool shutting_down;
      {
        Lock_guard incomplete_sessions_lock{m_state->m_mutex};
        if (!(shutting_down = m_state->m_stopping))
        {
#ifndef NDEBUG
          const bool erased_ok = 1 ==
#endif
          m_state->m_incomplete_sessions.erase(incomplete_session);
          assert(erased_ok && "Who else would have erased it?!");

          // Register as in-flight (see m_n_handlers_active); undone by handler_done() as our very last act.
          ++m_state->m_n_handlers_active;
        }
      }

      if (shutting_down)
      {
        /* As promised by our dtor contract: pending handlers fire with operation-aborted -- even if this log-in
         * happened to complete (successfully or not) just as shutdown began.  *target_session is untouched.
         * incomplete_session (our ref) is not the last one (m_incomplete_sessions still holds one), so letting it
         * go out of scope here is fine. */
        on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
        return;
      }
      // else

      if (async_err_code)
      {
        /* See class doc header.  We are in thread Ws; letting incomplete_session (the shared_ptr, now the last ref)
         * be destroyed here would cause it to try to join thread Ws which would deadlock; we'd be breaking the
         * contract to never destroy Server_session from its own handler.  So hand it off to this very-idle thread
         * to do it asynchronously.  (It is alive: dtor_stop_accepting() destroys it only after every thread Ws is
         * joined.) */
        m_state->m_incomplete_session_graveyard->post([incomplete_session = std::move(incomplete_session)]
                                                        () mutable
        {
          // That's that.  ~Server_session() will run here:
          incomplete_session.reset(); // Just in case compiler wants to warn about unused-var or optimize it away...?
        });
        // incomplete_session (the shared_ptr) is hosed at this point.
        assert((!incomplete_session) && "It should've been nullified by being moved-from into the captures.");

        /* As in the acceptor failure case above just forward it to handler.  All the same comments apply,
         * except there is no subtlety about operation-aborted coming from outside ipc::session.
         * If you change this, please consider synchronizing with the async_accept() handler above. */
        on_done_func(async_err_code);
      }
      else // if (!async_err_code)
      {
        assert(incomplete_session);

        // Yay!  Give it to the user.
        FLOW_LOG_INFO("Session acceptor [" << *this << "]: Async-accept request: Successfully resulted in logged-in "
                      "server session [" << *incomplete_session << "].  Feeding to user via callback.");

        *target_session = std::move(*incomplete_session);
        // *incomplete_session is now as-if default-cted.

        on_done_func(async_err_code); // Pass in Error_code{}.
        FLOW_LOG_TRACE("Handler finished.");

        /* (incomplete_session -- our ref, now the last -- is destroyed when we return, in thread Ws.  That is fine:
         * *incomplete_session is as-if default-cted (no impl), so its dtor joins nothing and touches nothing of
         * ours.) */
      } // else // if (!async_err_code)

      // As the very last act of a not-shutting_down path do the following; after, `*this` may be destroyed at any time.
      Lock_guard incomplete_sessions_lock{m_state->m_mutex};
      if (--m_state->m_n_handlers_active == 0)
      {
        m_state->m_handlers_done.notify_all();
      }
    }); // incomplete_session->async_accept_log_in()
  }); // m_master_sock_acceptor->async_accept()
} // Session_server_impl::async_accept()

TEMPLATE_SESSION_SERVER_IMPL
size_t CLASS_SESSION_SERVER_IMPL::mq_msg_size_limit() const
{
  static_assert(Server_session_obj::S_MQS_ENABLED,
                "Nonsensical to call this for a Session_server concrete type that is not MQ-enabled.");

  return m_mq_msg_size_limit;
}

TEMPLATE_SESSION_SERVER_IMPL
void CLASS_SESSION_SERVER_IMPL::mq_msg_size_limit(size_t limit)
{
  static_assert(Server_session_obj::S_MQS_ENABLED,
                "Nonsensical to call this for a Session_server concrete type that is not MQ-enabled.");

  const auto prev_limit = m_mq_msg_size_limit;
  m_mq_msg_size_limit = flow::util::round_to_multiple(limit, flow::util::max_align_sz());
  /* Rounding it to the "worst" possible alignment might prevent some annoying pain in various places, like
   * interaction with capnp segment sizes and who knows what else.  Really can't hurt.  (x86-64 => this is 16 BTW.) */

  FLOW_LOG_INFO("MQ msg-size limit changed via user call: [" << prev_limit << "] => round-up[" << limit << "] = "
                "[" << m_mq_msg_size_limit << "]; 0 = we choose based on best struc::Channel perf.  "
                "This affects all subsequently created channels including in already-existing session(s) if any.");
}

TEMPLATE_SESSION_SERVER_IMPL
void CLASS_SESSION_SERVER_IMPL::to_ostream(std::ostream* os) const
{
  *os << '[' << m_srv_app_ref << "]@" << static_cast<const void*>(this);
}

TEMPLATE_SESSION_SERVER_IMPL
typename CLASS_SESSION_SERVER_IMPL::Session_server_obj* CLASS_SESSION_SERVER_IMPL::this_session_srv()
{
  return m_this_session_srv;
}

TEMPLATE_SESSION_SERVER_IMPL
template<typename Task>
void CLASS_SESSION_SERVER_IMPL::sub_class_set_deinit_func(Task&& task)
{
  m_deinit_func_or_empty = std::move(task);
}

TEMPLATE_SESSION_SERVER_IMPL
std::ostream& operator<<(std::ostream& os,
                         const CLASS_SESSION_SERVER_IMPL& val)
{
  val.to_ostream(&os);
  return os;
}

#undef CLASS_SESSION_SERVER_IMPL
#undef TEMPLATE_SESSION_SERVER_IMPL

} // namespace ipc::session
