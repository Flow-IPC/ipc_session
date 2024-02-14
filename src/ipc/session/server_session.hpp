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
#include "ipc/session/detail/server_session_impl.hpp"
#include <boost/move/make_unique.hpp>

namespace ipc::session
{

// Types.

/**
 * Implements Session concept on the Server_app end: a Session that is emitted in almost-PEER state by
 * local Session_server accepting a connection by an opposing `Client_session_mv::async_connect()`.
 * By "almost-PEER state" we mean that the user, upon obtaining a new `Server_session_mv`, must still call
 * init_handlers() to enter PEER state.  See overview of Session hierarchy
 * in namespace ipc::session doc header; then come back here if desired.
 *
 * It is unusual to use Server_session_mv template directly.  If you do wish to set up a server-side session peer,
 * and you do not require SHM support, then use #Server_session alias template; target it with a
 * Session_server::async_accept() call.  The server-specific API, particularly init_handlers(),
 * is in Server_session_mv and documented here.  The side-agnostic API --
 * active once PEER state is achieved -- is described by Session (concept) doc header and implemented concretely
 * by Session_mv which is our `public`, non-`virtual` super-class.
 *
 * If you do wish to set up a server-side session peer, but you *do* require SHM support, then use
 * shm::classic::Server_session or similar (for other SHM-provider(s)); target it with a
 * shm::classic::Session_server::async_accept() (or similar for other SHM-provider(s)).  However #Server_session
 * (hence Server_session_mv to which it aliases) is its super-class, and until PEER state is reached its
 * API (init_handlers()) remains the only relevant API to use.  Once Server_session_mv::init_handlers()
 * puts `*this` into PEER state, super-class Session_mv (= Session concept) API continues to be relevant.
 * Also in PEER state SHM-relevant additional API members (e.g. shm::classic::Session::app_shm()`) become of interest.
 * The last two sentences describe the situation identically for Client_session_mv as well (see its doc header
 * for context).
 *
 * Summary hierarchy (contrast with similar spot in Client_session_mv doc header):
 *   - Session_mv (Session concept impl)
 *   - ^-- Server_session_mv (adds init_handlers())
 *   - =alias= #Server_session (parameterizes by transport-configuring knobs = template params)
 *   - ^-- shm::classic::Session_mv
 *     (adds `session_shm()`...; suitable for transport::struc::Channel_base::Serialize_via_session_shm)
 *   - ^-- shm::classic::Server_session (parameterizes by aforementioned knobs = template params)
 *
 * We may refer to Server_session_mv as #Server_session below; particularly since it is likeliest used in that
 * form by the user.
 *
 * ### How to use ###
 * Per the Session concept a #Server_session is open/capable of operation when in PEER state only.
 * A publicly available #Server_session object is always in one of 2 states:
 *   - Almost-PEER.  At this stage you must call init_handlers() to enter PEER state.  Until then other APIs
 *     will no-op/return sentinel, and no handler will fire.
 *   - PEER.  At this stage it exactly implements the Session concept.  See Session_mv.  Reminder:
 *     It is not possible to exit PEER state.
 *
 * Once in PEER state #Server_session simply follows Session concept semantics.  At this stage our super-class
 * Session_mv implements that concept in particular.  See either doc header (Session, Session_mv).
 *
 * ### Error handling ###
 * Once in PEER state, error handling follows the Session concept (= Session_mv concrete class) doc header.
 * However, up to that point it can emit no errors.  You must call init_handlers() first to enter PEER state.
 *
 * @internal
 * ### Implementation design/rationale ###
 * See section in the same spot of Client_session_mv doc header.  A symmetrical situation (w/r/t pImpl-lite
 * design) occurs here.  But let's be explicit at the risk of some copy/pasting:
 *
 * Session_mv is the highest (`public`) super-class and begins the pImpl-lite-for-movability technique.
 * It pImpl-wraps the Session-concept impl methods/stuff.  Server_session_mv merely repeats the same
 * technique but on the API additions at its own level; in particular init_handlers().
 *
 * The impl object is available via Session_mv::impl() (which is `protected`).  What type does this object
 * have though?  Answer: `Server_session_impl_t`, our template param!  This shall be the proper `..._impl`
 * internal-use type that matches the level of API (`public` sub-class) the user chose:  E.g.:
 *   - Chose #Server_session => impl = Server_session_impl.
 *   - Chose shm::classic::Server_session => impl = shm::classic::Server_session_impl.
 *
 * So basically each of the public-API types visible to the user adds the pImpl-wrapping of the methods
 * of the `..._impl` type matching their name; and the public/movable-API hierarchy has a parallel
 * internal/non-movable hierarchy, both using `public`/non-`virtual` inheritance.
 *
 * ### Facade design ###
 * Session_server emits a Server_session_mv only once it's in almost-PEER state.  Before then it is dealing
 * with it privately.  Namely -- details are in Server_session_impl -- it must
 *   -# construct it;
 *   -# call async_accept_log_in() and await its success.
 *
 * These APIs are `protected` in Server_session_mv.  The detail/ sub-class Server_session_dtl exposes them publicly
 * (but only accessible, by convention, internally; namely by Session_server).
 *
 * As of this writing the SHM-enabled Session_server variants (e.g., shm::classic::Session_server) do not require
 * any additional internally-accessed APIs.  Instead they employ 3 "customization points" using which avoids
 * such a need; see Session_server_impl.  Therefore there is no detail/ facade beyond Server_session_dtl.
 *
 * @see Server_session_impl doc header.
 * @endinternal
 *
 * @see Session: implemented concept.
 *
 * @tparam Server_session_impl_t
 *         An implementation detail.  Use one of the aliases prescribed near the top of this doc header to
 *         set this correctly.
 */
template<typename Server_session_impl_t>
class Server_session_mv :
  public Session_mv<Server_session_impl_t>
{
public:
  // Types.

  /// Short-hand for our base class.  To the user: note its `public` API is inherited.
  using Base = Session_mv<Server_session_impl_t>;

  // Constructors/destructor.

  /// Inherit all ctors from Session_mv (default, move).
  using Base::Base;

  // Methods.

  /**
   * The opposing application is described by a Client_app; this is that description.  May be useful in particular
   * when deciding what handlers to set up in init_handlers() when entering PEER state.  Returns null if `*this`
   * is as-if default-cted (i.e., default-cted + not moved-to otherwise; or moved-from).
   *
   * @return Pointer to immutable Client_app; or null if `*this` is as-if default-cted.  The pointer returned,
   *         when not straddling a move-to or move-from, is always the same.
   */
  const Client_app* client_app() const;

  /**
   * To be invoked by public user upon first obtaining `*this`: memorizes the given on-error and on-passive-open
   * handlers thus moving this Server_session_mv to PEER state wherein it is a formal Session concept impl.
   *
   * Using this overload indicates passive-opens are enabled on this side.
   *
   * Suggestion: use client_app() to determine the opposing Client_app, particularly if this Server_app is designed
   * to accept sessions from 2+ `Client_app`s.  (To be clear: Multiple *instances* (processes) of a *given* Client_app
   * are always supported.  However a given Server_app -- specified at Session_server construction time --
   * may well specify only one allowed opposing Client_app.)  It is likely that a different on-passive-open handler
   * would be useful depending on Client_app; possibly also different on-error handler.
   *
   * @tparam Task_err
   *         See Session concept doc header for semantics.
   * @tparam On_passive_open_channel_handler
   *         See Session concept doc header for semantics.
   * @param on_err_func_arg
   *        On-error handler per semantics in Session concept doc header.
   * @param on_passive_open_channel_func_arg
   *        On-passive-open handler per semantics in Session concept doc header.
   * @return `true` normally; `false` if invoked after already having called an init_handlers() or as-if default-cted.
   */
  template<typename Task_err, typename On_passive_open_channel_handler>
  bool init_handlers(Task_err&& on_err_func_arg, On_passive_open_channel_handler&& on_passive_open_channel_func_arg);

  /**
   * Alternative to the other init_handlers().  Using this overload indicates passive-opens are disabled on this side.
   * Otherwise identical to the other init_handlers() overload.
   *
   * @tparam Task_err
   *         See other init_handlers().
   * @param on_err_func_arg
   *        See other init_handlers().
   * @return `true` normally; `false` if invoked after already having called an init_handlers() or as-if default-cted.
   */
  template<typename Task_err>
  bool init_handlers(Task_err&& on_err_func_arg);

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using Base::get_logger;
  using Base::get_log_component;

protected:
  // Constructors.

  /**
   * For use by internal user Session_server_impl: constructor.  Invoke async_accept_log_in() to move forward
   * toward PEER state.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param srv_app_ref
   *        Properties of this server application.  The address is copied; the object is not copied.
   * @param master_channel_sock_stm
   *        The PEER-state transport::sync_io::Native_socket_stream that just connected to an opposing
   *        Client_session_impl.  It is moved-to `*this` (and hence becomes `.null()`).
   */
  explicit Server_session_mv(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                             transport::sync_io::Native_socket_stream&& master_channel_sock_stm);

  // Methods.

  /**
   * For use by internal user Session_server: called no more than once, ideally immediately following ctor,
   * this attempts to get `*this` asynchronously to almost-PEER state by undergoing the log-in request/response
   * (plus, if needed, init-channel-opening) procedure (the other side of which is done by
   * Client_session_impl::async_connect()).  On success, `on_done_func(Error_code())` is invoked from unspecified
   * thread that is not the user's calling thread.  On failure, it does similarly but with a non-success code.
   * If the op does not complete before dtor, then
   * `on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)` is invoked at that point.
   *
   * In the success-case firing, `*this` is ready to be given to the public user.  However it is in almost-PEER state.
   * To achieve PEER state they shall first call init_handlers().
   *
   * #Error_code generated and passed to `on_done_func()`:
   * session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (see above),
   * those returned by transport::Native_socket_stream::remote_peer_process_credentials(),
   * those emitted by transport::struc::Channel::send(),
   * those emitted by transport::struc::Channel via on-error handler (most likely
   * transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE indicating graceful shutdown of opposing process
   * coincidentally during log-in procedure, prematurely ending session while it was starting),
   * error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN,
   * error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS,
   * those emitted by `pre_rsp_setup_func()` (in-arg).
   *
   * @note If the above list substantially changes, please update Session_server::async_accept()
   *       doc header accordingly for user convenience.
   *
   * @tparam Session_server_impl_t
   *         See `srv`.
   * @tparam Task_err
   *         Handler type matching signature of `flow::async::Task_asio_err`.
   * @tparam Cli_app_lookup_func
   *         Function type that takes a supposed Client_app::m_name and returns the `const Client_app*` with that
   *         matching name; or null if it is not a validly registered name according to the data given to the
   *         internal user.
   * @tparam Cli_namespace_func
   *         Function type that returns a sufficiently-distinct value for Session_base::cli_namespace(),
   *         as Session_base::set_cli_namespace() is required before #Server_session can be in PEER state and available
   *         to the public user.
   * @tparam Pre_rsp_setup_func
   *         Function type with signature `Error_code F()`.
   * @tparam N_init_channels_by_srv_req_func
   *         See Session_server::async_accept().  Type and arg value forwarded from there.
   * @tparam Mdt_load_func
   *         See Session_server::async_accept().  Type and arg value forwarded from there.
   * @param srv
   *        The Session_server_impl whose Session_server_impl::async_accept() is invoking the present method.
   *        `*srv` must exist at least while `*this` does, or behavior is undefined.
   *        This allows for interaction/cooperation with the "parent" `Session_server` if necessary, such as
   *        for shared cross-session resources.
   *        Note: you may use `srv->this_session_srv()` to obtain a pointer to the `Session_server`-like object
   *        on which the user invoked `async_accept()`.  E.g.: Server_session_impl::async_accept_log_in()
   *        would get a `Session_server*`; shm::classic::Server_session_impl::async_accept_log_in() would get
   *        a `shm::classic::Session_server*`.
   * @param init_channels_by_srv_req
   *        See Session_server::async_accept().  Arg value forwarded from there.
   * @param mdt_from_cli_or_null
   *        See Session_server::async_accept().  Arg value forwarded from there.
   * @param init_channels_by_cli_req
   *        See Session_server::async_accept().  Arg value forwarded from there.
   * @param cli_app_lookup_func
   *        See `Cli_app_lookup_func`.
   * @param cli_namespace_func
   *        See `Cli_namespace_func`.
   * @param pre_rsp_setup_func
   *        Invoked just before sending successful log-in response to opposing client, which completes the log-in.
   *        It takes no arguments -- but all `*this` accessors up to/including `cli_app_ptr()` shall return
   *        real values -- and shall return falsy on success; or the reason for failure as #Error_code.
   *        In the latter case this method shall emit that code as the reason for overall failure.
   *        This can be used for setting up resources, such as SHM arena(s), that the client shall count
   *        on being available (perhaps at known #Shared_name based on `*this` accessor values).
   * @param n_init_channels_by_srv_req_func
   *        See `N_init_channels_by_srv_req_func`.
   * @param mdt_load_func
   *        See `Mdt_load_func`.
   * @param on_done_func
   *        See above.
   */
  template<typename Session_server_impl_t,
           typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept_log_in(Session_server_impl_t* srv,
                           typename Base::Channels* init_channels_by_srv_req,
                           typename Base::Mdt_reader_ptr* mdt_from_cli_or_null,
                           typename Base::Channels* init_channels_by_cli_req,
                           Cli_app_lookup_func&& cli_app_lookup_func, Cli_namespace_func&& cli_namespace_func,
                           Pre_rsp_setup_func&& pre_rsp_setup_func,
                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                           Mdt_load_func&& mdt_load_func,
                           Task_err&& on_done_func);
}; // class Server_session_mv

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SRV_SESSION_MV \
  template<typename Server_session_impl_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SRV_SESSION_MV \
  Server_session_mv<Server_session_impl_t>

// The rest is strict forwarding to impl() (when not null).

TEMPLATE_SRV_SESSION_MV
CLASS_SRV_SESSION_MV::Server_session_mv(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                                        transport::sync_io::Native_socket_stream&& master_channel_sock_stm)
{
  Base::impl() = boost::movelib::make_unique<Server_session_impl_t>(logger_ptr, srv_app_ref,
                                                                    std::move(master_channel_sock_stm));
}

TEMPLATE_SRV_SESSION_MV
const Client_app* CLASS_SRV_SESSION_MV::client_app() const
{
  return Base::impl() ? Base::impl()->client_app() : nullptr;
}

TEMPLATE_SRV_SESSION_MV
template<typename Session_server_impl_t,
         typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_SRV_SESSION_MV::async_accept_log_in
       (Session_server_impl_t* srv,
        typename Base::Channels* init_channels_by_srv_req,
        typename Base::Mdt_reader_ptr* mdt_from_cli_or_null,
        typename Base::Channels* init_channels_by_cli_req,
        Cli_app_lookup_func&& cli_app_lookup_func,
        Cli_namespace_func&& cli_namespace_func,
        Pre_rsp_setup_func&& pre_rsp_setup_func,
        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
        Mdt_load_func&& mdt_load_func,
        Task_err&& on_done_func)
{
  assert(Base::impl() && "Session default ctor is meant only for being moved-to.  Internal bug?");
  Base::impl()->async_accept_log_in(srv,
                                    init_channels_by_srv_req,
                                    mdt_from_cli_or_null,
                                    init_channels_by_cli_req,
                                    std::move(cli_app_lookup_func),
                                    std::move(cli_namespace_func),
                                    std::move(pre_rsp_setup_func),
                                    std::move(n_init_channels_by_srv_req_func),
                                    std::move(mdt_load_func),
                                    std::move(on_done_func));
}

TEMPLATE_SRV_SESSION_MV
template<typename Task_err, typename On_passive_open_channel_handler>
bool CLASS_SRV_SESSION_MV::init_handlers(Task_err&& on_err_func_arg,
                                         On_passive_open_channel_handler&& on_passive_open_channel_func_arg)
{
  return Base::impl()
           ? Base::impl()->init_handlers(std::move(on_err_func_arg), std::move(on_passive_open_channel_func_arg))
           : false;
}

TEMPLATE_SRV_SESSION_MV
template<typename Task_err>
bool CLASS_SRV_SESSION_MV::init_handlers(Task_err&& on_err_func_arg)
{
  return Base::impl() ? Base::impl()->init_handlers(std::move(on_err_func_arg)) : false;
}

TEMPLATE_SRV_SESSION_MV
std::ostream& operator<<(std::ostream& os, const CLASS_SRV_SESSION_MV& val)
{
  return os << static_cast<const typename CLASS_SRV_SESSION_MV::Base&>(val);
}

#undef CLASS_SRV_SESSION_MV
#undef TEMPLATE_SRV_SESSION_MV

} // namespace ipc::session
