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

#include "ipc/session/session.hpp"
#include "ipc/session/detail/client_session_impl.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session
{

// Types.

/**
 * Implements Session concept on the Client_app end: a Session_mv that first achieves PEER state by connecting
 * to an opposing Session_server via Client_session_mv::sync_connect().  See overview of Session hierarchy
 * in namespace ipc::session doc header; then come back here if desired.
 *
 * It is unusual to use Client_session_mv template directly.  If you do wish to set up a client-side session peer,
 * and you do not require SHM support, then use #Client_session alias template.  The client-specific API,
 * particularly ctor and sync_connect(), is in Client_session_mv and documented here.  The side-agnostic API --
 * active once PEER state is achieved -- is described by Session (concept) doc header and implemented concretely
 * by Session_mv which is our `public`, non-`virtual` super-class.
 *
 * If you do wish to set up a client-side session peer, but you *do* require SHM support, then use
 * shm::classic::Client_session or similar (for other SHM-provider(s)).  However #Client_session
 * (hence Client_session_mv to which it aliases) is its super-class, and until PEER state is reached its API
 * remains the only relevant API to use.  Once Client_session_mv::sync_connect() puts `*this` into PEER
 * state, super-class Session_mv (= Session concept) API continues to be relevant.  Also in PEER state
 * SHM-relevant additional API members (e.g., shm::classic::Session_mv::app_shm()) become of interest.
 *
 * Summary hierarchy:
 *   - Session_mv (Session concept impl)
 *   - ^-- Client_session_mv (adds sync_connect())
 *   - =alias= #Client_session (parameterizes by transport-configuring knobs = template params)
 *   - ^-- shm::classic::Session_mv
 *     (adds `session_shm()`...; suitable for transport::struc::Channel_base::Serialize_via_session_shm)
 *   - =alias= shm::classic::Client_session (parameterizes by aforementioned knobs = template params)
 *
 * We may refer to Client_session_mv as #Client_session below; particularly since it is likeliest used in that
 * form by the user.
 *
 * ### How to use ###
 * Per the Session concept a #Client_session is open/capable of operation when in PEER state only.
 * A #Client_session object is always in one of 2 states:
 *   - NULL.  In this state, the object is doing nothing; neither connected nor connecting.
 *   - PEER.  At this stage it exactly implements the Session concept.  See Session_mv.  Reminder:
 *     It is not possible to exit PEER state.
 *
 * To get from NULL state to PEER state: Use Session_server on the other side.  On this side construct
 * a #Client_session (always in NULL state) and invoke sync_connect() which will synchronously and non-blockingly try
 * to move to PEER state (mission accomplished).  If the connect fails, it will stay in NULL
 * state.  Being moved-from makes a `*this` as-if default-cted; therefore also moves to NULL state.
 *
 * Once in PEER state #Client_session simply follows Session concept semantics.  At this stage our super-class
 * Session_mv implements that concept in particular.  See either doc header (Session, Session_mv).
 *
 * A related reminder:
 *   - sync_connect() or the opposing `async_accept()` or both may optionally specify that 1+ *init-channels*
 *     be opened before this and the opposing Session is in PEER state and emitted to user.
 *     This may be a sufficient replacement of, or complementary to, active-open/passive-open APIs available
 *     once the Session is in PEER state/emitted to user.  If not sufficient:
 *   - Either side (being a Session_mv = Session) has open_channel(), and hence either side can accept passive-opens
 *     when the opposing side invokes open_channel().
 *   - However it is possible to configure either side to not accept passive-opens; meaning to be useful
 *     such a side shall be exclusively doing open_channel() (active-opens).  One might conclude, then, that
 *     #Server_session should be passive-opening, while #Client_session should be active-opening.
 *     That is *not* the case.  In PEER state established on both sides, the 2 sides are identical in this
 *     respect.  If both sides are configured to accept passive-opens, then there's no question.
 *     If one side is so configured, it can be either side.  If both sides are so configured, the session is useless,
 *     as every open_channel() will fail (unless one has set up init-channels).
 *     - In #Client_session, one specifies the on-passive-open handler in ctor; to disable passive-opens use
 *       the ctor that omits that handler.
 *
 * ### Why no async-connect method, only sync-connect? ###
 * Without networking, the other side (Session_server) either exists/is listening; or no.
 * Connecting is a synchronous, non-blocking operation; so an `async_connect()` API in this context only makes
 * life harder for the user.  (However, there are some serious plans to add a networking-capable counterpart
 * (probably via TCP at least) to `Native_socket_stream` and therefore Client_session_mv; such a networky-client-session
 * class will almost certainly have an `async_connect()`, while its `sync_connect()` will probably become potentially
 * blocking.)
 *
 * @note Impl detail for exposition purposes: transport::Native_socket_stream is used internally by ipc::session
 *       to establish internal session-related IPC.  That is a Unix domain socket: local stream connection.  This can be
 *       extended, relatively cheaply in terms of effort, to work over networked-TCP sockets.  Based on that work,
 *       Client_session and Session_server hierarchies can be relatively cheaply extended to also be network-enabled in
 *       some form.
 *
 * @note If some form of `*this` becomes network-enabled, open_channel() too will need an async form most likely,
 *       while existing open_channel() would become potentially-blocking.
 *
 * ### Thread safety and handler invocation semantics ###
 * These follow the Session concept policies.

 * ### Error handling ###
 * Once in PEER state, error handling follows the Session concept (= Session_mv concrete class) doc header.
 * Up to that point (in NULL state) there are no public asynchronous operations; there is just sync_connect() which
 * succeeds or fails synchronously and quickly.
 *
 * @internal
 * ### Implementation design/rationale ###
 * Session_mv is the highest (`public`) super-class and begins the pImpl-lite-for-movability technique.
 * It pImpl-wraps the Session-concept impl methods/stuff.  Client_session_mv merely repeats the same
 * technique but on the API additions at its own level; in particular the `explicit` ctors and
 * especially sync_connect().
 *
 * The impl object is available via Session_mv::impl() (which is `protected`).  What type does this object
 * have though?  Answer: `Client_session_impl_t`, our template param!  This shall be the proper `..._impl`
 * internal-use type that matches the level of API (`public` sub-class) the user chose:  E.g.:
 *   - Chose #Client_session => impl = Client_session_impl.
 *   - Chose shm::classic::Client_session => impl = shm::classic::Client_session_impl.
 *
 * So basically each of the public-API types visible to the user adds the pImpl-wrapping of the methods
 * of the `..._impl` type matching their name; and the public/movable-API hierarchy has a parallel
 * internal/non-movable hierarchy, both using `public`/non-`virtual` inheritance.
 *
 * @see Client_session_impl doc header.
 * @endinternal
 *
 * @see Session: implemented concept.
 *
 * @tparam Client_session_impl_t
 *         An implementation detail.  Use one of the aliases prescribed near the top of this doc header to
 *         set this correctly.
 */
template<typename Client_session_impl_t>
class Client_session_mv :
  public Session_mv<Client_session_impl_t>
{
public:
  // Types.

  /// Short-hand for our base class.  To the user: note its `public` API is inherited.
  using Base = Session_mv<Client_session_impl_t>;

  // Constructors/destructor.

  /// Inherit all ctors from Session_mv (default, move).
  using Base::Base;

  /**
   * Constructs (passive-opens allowed form) in NULL state.  To be useful, invoke sync_connect() next.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param cli_app_ref
   *        Properties of this client application.  The address is copied; the object is not copied.
   * @param srv_app_ref
   *        Properties of the opposing server application.  The address is copied; the object is not copied.
   * @param on_err_func
   *        On-error handler.  See Session concept doc header for semantics.
   * @param on_passive_open_channel_func
   *        On-passive-open handler.  See Session concept doc header for semantics.
   * @tparam On_passive_open_channel_handler
   *         See Session concept doc header for semantics.
   * @tparam Task_err
   *         See Session concept doc header for semantics.
   */
  template<typename On_passive_open_channel_handler, typename Task_err>
  explicit Client_session_mv(flow::log::Logger* logger_ptr,
                             const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                             Task_err&& on_err_func,
                             On_passive_open_channel_handler&& on_passive_open_channel_func);

  /**
   * Constructs (passive-opens disallowed form) in NULL state.  To be useful, invoke sync_connect() next.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param cli_app_ref
   *        Properties of this client application.  The address is copied; the object is not copied.
   * @param srv_app_ref
   *        Properties of the opposing server application.  The address is copied; the object is not copied.
   * @param on_err_func
   *        On-error handler.  See Session concept doc header for semantics.
   * @tparam Task_err
   *         See Session concept doc header for semantics.
   */
  template<typename Task_err>
  explicit Client_session_mv(flow::log::Logger* logger_ptr,
                             const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                             Task_err&& on_err_func);

  // Methods.

#ifdef IPC_DOXYGEN_ONLY // Actual compilation will ignore the below; but Doxygen will scan it and generate docs.
  /**
   * Implements Session API per contract; plus usable to generate blank `mdt` argument for advanced
   * sync_connect() overload.  That is: can additionally be invoked in NULL state (before sync_connect() or
   * between a failed attempt and the next attempt).  The return value can be used only as follows; otherwise
   * undefined behavior results:
   *   - If invoked in NULL state: pass to sync_connect().
   *   - If invoked in PEER state: pass to open_channel().
   *
   * @return See above.
   *
   * @see Session::mdt_builder(): implemented concept.
   */
  Mdt_builder_ptr mdt_builder();

#endif // IPC_DOXYGEN_ONLY

  /**
   * To be invoked in NULL state only, and not as-if default-cted, it synchronously and non-blockingly attempts to
   * connect to an opposing `Session_server`; and synchronously reports failure or success, the latter
   * showing `*this` has entered PEER state.  Failure means `*this` remains in NULL state.
   *
   * If invoked outside of NULL state, or if it is as-if default-cted (i.e., default-cted or moved-from),
   * this returns `false` and otherwise does nothing.
   *
   * Note that `*this` (modulo moves) that has entered PEER state can never change state subsequently
   * (even on transmission error); once a PEER, always a PEER.
   *
   * ### How does it know "where" the Session_server is listening? ###
   * While details are internal, abstractly speaking this is determined by the Server_app passed to the ctor.
   * Internal detail (for exposition/general knowledge):
   *   - The server application maintains a *CNS* (Current Namespace Store, a/k/a PID) file.
   *     Only at most 1 server *instance* (a/k/a process) shall be active at a time.  The CNS contents --
   *     which are simply the active server app process's PID -- determine "where" this `Client_session`
   *     will connect, and all other internally used cross-process resources
   *     (sockets, MQs, SHM pools...) are similarly segregated among different instances of the Server_app-described
   *     application.
   *   - The location of this file is determined by Server_app contents.  Namely, Server_app::m_name, at least,
   *     is unique per Server_app in the universe, and most of the PID file's name comprises it; and
   *     its location comprises Server_app::m_kernel_persistent_run_dir_override and/or /var/run (or equivalent).
   *   - sync_connect() reads the CNS (PID) file at that location, unique among `Server_app`s.  Now it knows
   *     "where" to connect in order to open the session; so it won't accidentally connect to some old zombie
   *     instance of the Server_app (its PID wouldn't be in the CNS file any longer) or another application entirely
   *     (its PID file would be located elsewhere and never accessed by this sync_connect()).
   *
   * ### What if the CNS (PID) file does not exist?  What if server is inactive? ###
   * Then sync_connect() will quickly fail (file-not-found or connection-refused error).  Shouldn't we supply some
   * API to await its appearance and actively listening?  Answer: Probably not.  Why? 
   * Answer: Consider the situation where the PID file is there, but
   * it belongs to a now-exited instance of the Server_app; let's say it is currently suspended or restarting.
   * sync_connect() will fail then too: There is nothing listening (anymore) at the advertised PID.  So
   * it'll quickly fail (connection-refused error).  So then we'd also need an API to await an active server's
   * appearance whether or not the PID exists.  That leads to the following to-do:
   *
   * @todo Consider adding an optional mode/feature to allow to wait through the condition wherein CNS (PID) file does
   * not exist, or it does but the initial session-open connect op would be refused; instead it detects these relatively
   * common conditions (server not yet up and/or is restarting and/or is operationally suspended for now, etc.) as
   * normal and waits until the condition is cleared.  Without this mode, a typical user would probably do
   * something like: oh, sync_connect() failed; let's sleep 1 sec and try again (rinse/repeat).  It's not
   * awful, but we might as well make it easier and more responsive out of the box (optionally).  Upon resolving this
   * to-do please update the Manual section @ref session_setup accordingly.
   *
   * @see Class doc header, section "Why no async-connect method, only sync-connect?" for potentially interesting
   *      context.
   *
   * @return `false` if and only if invoked outside of NULL state; or if `*this` is as-if default-cted.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   *        spiritually identical to `boost::asio::error::operation_aborted`),
   *        interprocess-mutex-related errors (probably from boost.interprocess) w/r/t reading the CNS (PID file),
   *        file-related system errors w/r/t reading the CNS (PID file) (see Session_server doc header for background),
   *        error::Code::S_CLIENT_NAMESPACE_STORE_BAD_FORMAT (bad CNS contents),
   *        those emitted by transport::Native_socket_stream::sync_connect(),
   *        those emitted by transport::struc::Channel::send(),
   *        those emitted by transport::struc::Channel via on-error handler (most likely
   *        transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE indicating graceful shutdown of opposing process
   *        coincidentally during log-in procedure, prematurely ending session while it was starting),
   *        error::Code::S_CLIENT_MASTER_LOG_IN_RESPONSE_BAD,
   *        error::Code::S_INVALID_ARGUMENT (other side expected other sync_connect() overload with
   *        non-null `init_channels_by_srv_req` arg).
   */
  bool sync_connect(Error_code* err_code = 0);

  /**
   * Identical to the simpler sync_connect() overload but offers added advanced capabilities: metadata exchange;
   * initial-channel opening.  The other overload is identical to
   * `sync_connect(mdt_builder(), nullptr, nullptr, nullptr, err_code)` and requires
   * the opposing `async_accept()` to similarly not use the corresponding features.
   *
   * ### Client->server metadata exchange ###
   * Client may wish to provide information in some way, but without a structured channel -- or any channel -- yet
   * available, this provides an opportunity to do so in a structured way with a one-off message available at
   * session open, together with the opposing Session object itself.
   *
   * Similarly to how it is optionally done on a per-channel-open basis (open_channel()): call `mdt = mdt_builder()`;
   * fill out the resulting capnp structure `*mdt`; then pass `mdt` to sync_connect().  Opposing `async_accept()` shall
   * receive deserializable `Mdt_reader` together with the freshly-opened Session, though they may choose to ignore it.
   *
   * To omit using this feature, skip the "fill out the resulting capnp structure" step.  You must still
   * call mdt_builder() and pass-in the result.  (The other overload does so.)
   *
   * ### Server->client metadata exchange ###
   * This is the reverse of the above.  Whatever the opposing server chose to supply as server->client metadata
   * shall be deserializable at `*mdt_from_srv_or_null` on successful return from `sync_connect()`.
   * If `mdt_from_srv_or_null` is null, the srv->cli metadata shall be ignored.
   *
   * ### Init-channels by client request ###
   * Once the session is open, open_channel() and the on-passive-open handler may be used to open channels at will.
   * In many use cases, however, a certain number of channels is required immediately before work can really begin
   * (and, frequently, no further channels are even needed).  For convenience (to avoid asynchrony/boiler-plate)
   * the init-channels feature will pre-open a requested # of channels making them available right away, together
   * with the freshly-open session -- to both sides.
   *
   * The client may request 0 or more init-channels.  They shall be opened and placed into
   * `*init_channels_by_cli_req_pre_sized`; its `->size()` at entry to the method indicates the number of channels
   * requested.  If null, it is treated as if `->size() == 0`, meaning no init-channels-by-client-request are opened.
   *
   * ### Init-channels by server request ###
   * This is the reverse of the above.  The opposing side shall request 0 or more init-channels-by-server-request;
   * that number of channels shall be opened; and they will be placed into `*init_channels_by_srv_req` which shall
   * be `->resize()`d accordingly on successful return from `sync_connect()`.
   *
   * `init_channels_by_srv_req` being null is allowed, but only if the opposing server requests 0
   * init-channels-by-server-request.  Otherwise an error shall be emitted (see below).
   *
   * @param mdt
   *        See above.
   * @param init_channels_by_cli_req_pre_sized
   *        See above: null or pointer to container of `Channel_obj` with `.size()` specifying how many channels
   *        this side is requesting to be opened on its behalf; each element will be move-assigned
   *        a PEER-state `Channel_obj` on success.  Recommend simply ct-ing with `(n)` or `.resize(n)` which
   *        loads it with default-cted (NULL-state) objects to be replaced.  null is treated same as
   *        `.empty()`.
   * @param mdt_from_srv_or_null
   *        See above: null or pointer to `Reader` of metadata which shall be set for access on success.
   * @param init_channels_by_srv_req
   *        See above: null or pointer to container of `Channel_obj` which shall be `.clear()`ed and replaced
   *        by a container of PEER-state `Channel_obj` on success the number being specified by the opposing
   *        (server) side.  The number may be zero.  null is allowed if and only if the number is zero;
   *        otherwise error::Code::S_INVALID_ARGUMENT is emitted.
   * @param err_code
   *        See other sync_connect() overload.  Note the above target (pointer) args are touched
   *        only if no error is emitted via this standard Flow error reporting mechanism.
   * @return See other sync_connect() overload.
   */
  bool sync_connect(const typename Base::Mdt_builder_ptr& mdt,
                    typename Base::Channels* init_channels_by_cli_req_pre_sized = 0,
                    typename Base::Mdt_reader_ptr* mdt_from_srv_or_null = 0,
                    typename Base::Channels* init_channels_by_srv_req = 0,
                    Error_code* err_code = 0);


  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using Base::get_logger;
  using Base::get_log_component;
}; // class Client_session_mv

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLI_SESSION_MV \
  template<typename Client_session_impl_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLI_SESSION_MV \
  Client_session_mv<Client_session_impl_t>

// The rest is strict forwarding to impl() (when not null).

TEMPLATE_CLI_SESSION_MV
template<typename On_passive_open_channel_handler, typename Task_err>
CLASS_CLI_SESSION_MV::Client_session_mv
  (flow::log::Logger* logger_ptr,
   const Client_app& cli_app_ref, const Server_app& srv_app_ref,
   Task_err&& on_err_func,
   On_passive_open_channel_handler&& on_passive_open_channel_func_or_empty_arg)
{
  Base::impl()
    = boost::movelib::make_unique<Client_session_impl_t>(logger_ptr, cli_app_ref, srv_app_ref,
                                                         std::move(on_err_func),
                                                         std::move(on_passive_open_channel_func_or_empty_arg));
}

TEMPLATE_CLI_SESSION_MV
template<typename Task_err>
CLASS_CLI_SESSION_MV::Client_session_mv(flow::log::Logger* logger_ptr,
                                        const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                        Task_err&& on_err_func)
{
  Base::impl()
    = boost::movelib::make_unique<Client_session_impl_t>(logger_ptr, cli_app_ref, srv_app_ref,
                                                         std::move(on_err_func));
}

TEMPLATE_CLI_SESSION_MV
bool CLASS_CLI_SESSION_MV::sync_connect(Error_code* err_code)
{
  return Base::impl() ? Base::impl()->sync_connect(err_code) : false;
}

TEMPLATE_CLI_SESSION_MV
bool CLASS_CLI_SESSION_MV::sync_connect(const typename Base::Mdt_builder_ptr& mdt,
                                        typename Base::Channels* init_channels_by_cli_req_pre_sized,
                                        typename Base::Mdt_reader_ptr* mdt_from_srv_or_null,
                                        typename Base::Channels* init_channels_by_srv_req,
                                        Error_code* err_code)
{
  return Base::impl()
           ? Base::impl()->sync_connect(mdt, init_channels_by_cli_req_pre_sized,
                                        mdt_from_srv_or_null, init_channels_by_srv_req, err_code)
           : false;
}

TEMPLATE_CLI_SESSION_MV
std::ostream& operator<<(std::ostream& os, const CLASS_CLI_SESSION_MV& val)
{
  return os << static_cast<const typename CLASS_CLI_SESSION_MV::Base&>(val);
}

#undef CLASS_CLI_SESSION_MV
#undef TEMPLATE_CLI_SESSION_MV

} // namespace ipc::session
