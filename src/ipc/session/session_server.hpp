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

#include "ipc/session/server_session.hpp"
#include "ipc/session/detail/session_server_impl.hpp"

namespace ipc::session
{

// Types.

/**
 * To be instantiated typically once in a given process, an object of this type asynchronously listens for
 * Client_app processes each of which wishes to establish a *session* with this server process;
 * emits resulting Server_session objects locally.
 *
 * ### When to use ###
 * In the ipc::session paradigm: See #Server_session doc header "When to use" section.  If and only if,
 * based on that text, you wish to use this application to instantiate `Server_session`s, then you must
 * instantiate a single Session_server to emit them.  (It is not possible to directly construct
 * a PEER-state -- i.e., at all useful -- #Server_session.)
 *
 * Note that while in a typical (non-test/debug) scenario one would only instantiate a single
 * Session_server, that does *not* mean it cannot or should not instantiate `Client_session`s as well.
 * In a *given* split, a given application is either the server or the client; but in a different split it
 * be the other thing.  So a Session_server handles all the splits in which this application is the
 * server; but for other splits one would instantiate 0+ `Client_session`s separately.
 *
 * ### How to use ###
 * Similarly to transport::Native_socket_stream_acceptor constructing `*this` immediately async-listens to
 * incoming connections (each an attempt to establish a opposing Client_session to partner with a new local
 * #Server_session).  async_accept() shall accept such connections, one at a time, in FIFO order (including any
 * that may have queued up in the meantime, including after mere construction).  The meaning of FIFO order
 * is discussed below.
 *
 * Once a #Server_session is emitted to a handler passed to async_accept(), refer to #Server_session doc header
 * for how to operate it.  Spoiler alert: you must still call `Server_session::init_handlers()`; at which point
 * the #Server_session is a Session concept impl in PEER state.
 *
 * ### Error handling ###
 * The model followed is the typical boost.asio-like one: Each async_accept() takes a handler, F, and it is
 * eventually invoked no matter what: either with a falsy (success) #Error_code, or with a truthy (fail) one.
 * If the op does not complete, and one invokes the `*this` dtor, then `F()` is invoked at dtor time with
 * a particular operation-aborted #Error_code.
 *
 * That said, unlike with (say) transport::Native_socket_stream_acceptor -- and many other classes in ::ipc
 * and boost.asio -- the failure of a given async_accept() does *not* formally hose `*this`.  Formally,
 * other async_accept(), including future ones, may succeed.  Rationale:
 *   - The high-level nature of this class is such that it seemed important to not disregard failed incoming
 *     connect attempts by simply not emitting them.  The user might monitor/report based on the nature
 *     of each problematic one.  In particular, log-in failures are of interest w/r/t safety + security.
 *   - Given that point, there is no reason to necessarily hose the entire IPC framework for this process (as a
 *     server) due to one badly behaving opposing client.
 *   - Is is unlikely that a Unix domain socket listener will fail, if it was able to begin listening.
 *     So post-ctor problems are unlikely to occur strictly before the log-in exchange procedure.
 *
 * Informally there are a few ways one could deal with each given error.
 *   - Simply treating *any* failed async_accept() as `*this`-hosing may well be reasonable.  If something
 *     went wrong, perhaps it is safest to exit and let the daemon restart in hopes things will start working.
 *   - Alternatively one can look for particular #Error_code values.  If it has to do with log-in
 *     continue trying `async_accept()`s -- perhaps up to a certain number of failures.  If it's something not
 *     specifically explainable, revert to the previous bullet point (die, restart).  The possible #Error_code
 *     values emitted by async_accept() are documented in its doc header.
 *
 * As of this writing I (ygoldfel) lack sufficient data in the field to make certain pronouncements.
 *
 * ### Relationship with Server_app and Client_app; relationship with file system ###
 * This class operates at quite a high level; there should be (outside of test/debug) only on of them in a process.
 * There are some important relationships to understand:
 *   - This process must be invoked consistently with the description in Server_app as passed to its ctor.
 *     See Server_app (and therefore App) doc header.  In particular its App::m_exec_path, App::m_user_id,
 *     App::m_group_id must be consistent with reality.
 *   - The Client_app::Master_set of all `Client_app`s must be equal between all applications (this one and
 *     the potential client apps included) in the relevant IPC universe -- equal by value and therefore
 *     with the same key set (key = App::m_name).
 *   - Similarly for Server_app::Master_set of all `Server_app`s.
 *   - Server_app::m_allowed_client_apps shall list all `Client_app`s that may indeed open sessions against
 *     this application.  Any other attempts will be rejected.
 *     - Furthermore each given client app instance (process) will need to know certain address information about
 *       this server process so as to connect to the right guy (not just application but instance -- e.g., to not
 *       connect to some old/dying/zombified instance).  While details are intentionally internal there is an
 *       interaction with the file system that you should know: The datum internally known as the *server app namespace*
 *       is the key thing that varies from instance to instance.  It is, in fact, the PID (process ID) which has
 *       good uniqueness properties.  This is saved in the so-called Current Namespace Store (CNS) which is
 *       simply a PID file, a fairly typical construct in Unix daemons.  Therefore:
 *       - Session_server writes this in its ctor.
 *       - The name is, as of this writing, X.pid, where X is Server_app::m_name.
 *       - The location is either a sensible default (/var/run as of this writing) or an override path
 *         Server_app::m_kernel_persistent_run_dir_override.  (Reminder: the same `Server_app`s, by value, must
 *         be registered in all processes, whether client or server or both.  Hence this override will be
 *         agreed-upon on both sides.)
 *       - Client_session::sync_connect() shall check this file immediately to know to which server process --
 *         Session_server -- to connect.
 *
 * As of this writing we expect Session_server to be in charge of maintaining the process's only
 * PID file, in the sense that a typical daemon is expected to maintain a general-use PID file, with the usual
 * format (where the file contains a decimal encoding of the PID plus a newline).  If this
 * is not acceptable -- i.e., we don't want the ::ipc system to "step" on some other mechanism for PID files
 * already in use -- then the location may be tweaked, at least via Server_app::m_kernel_persistent_run_dir_override
 * or possibly with a different default than the current choice (/var/run).
 *
 * ### Thread safety; handler invocation ###
 * After construction of `*this`, concurrent access to API methods and the dtor is not safe, unless all methods
 * being invoked concurrently are `const`.
 *
 * You *may* invoke async_accept() directly from within another async_accept()-passed handler.
 * (Informally we suggest you post all real handling onto your own thread(s).)
 *
 * However: async_accept()-passed user handlers may be invoked *concurrently to each other* (if 2+ async_accept()s are
 * outstanding).  It is your responsibility, then, to ensure you do not invoke 2+ async_accept()s concurrently.
 * (One way is to only have only 1 outstanding at any given time.  Another is to post all handling on your own thread.
 * Really informally we'd say it's best to do both.)
 *
 * ### Subtlety about "FIFO order" w/r/t async_accept() ###
 * Above we promise that async_accept() shall emit ready `Server_sessions`s in "FIFO" order.  The following
 * expands on that, for completeness, even though it is unlikely a typical user will care.  The FIFO order is subtly
 * different from that in, say, `Native_socket_stream_acceptor` or boost.asio's
 * transport::asio_local_stream_socket::Acceptor.  To wit:
 *
 * Suppose async-accept request R1 was followed by R2.  As a black box, it is possible that the handler for
 * R2 will fire before the handler for R1.  I.e., the "first out" refers to something different than
 * the order of emission.  Rather: each given request R consists of 2 steps internally (listed here in the
 * public section only for exposition):
 *   -# Accept a Unix domain socket connection.  Immediately upon it being accepted:
 *   -# Execute a log-in exchange on that connection, including things like Client_app identification and verification
 *      and a session token thing.
 *
 * The "FIFO order" refers to the first step.  Request R1 shall indeed handle the first incoming socket connection;
 * R2 the second: first-come, first-served.  However, the log-in exchange must still occur.  In practice this
 * should be quite quick: Client_session will immediately ping us, and we'll ping them back, and that's that.
 * However, it is possible that (for whatever reason) R2 will win the race against R1 despite a head start for
 * the latter.  Therefore, it won't look quite like FIFO, though probably it will -- and if not, then close enough.
 *
 * Rationale: Honestly: It really has to do with simplicity of implementation (discussed in the impl section below but
 * of little interest to the user and not included in the public-API Doxygen-generated docs).  Implementing it
 * that way was simpler and easier to maintain; probably the perf less latent too.  The fact that, as a result,
 * the FIFO order might get sometimes switched around did not seem important in practice.  Still it is worth
 * remarking upon formally.
 *
 * Informally: are there any practical concerns about this for the user?  It seems unlikely.  It is, one supposes,
 * sort-of conceivable that some system would rely on the ordering of incoming sessions being requested to be
 * significant in some larger inter-application algorithm.  However, 1, that sounds highly unorthodox to put it mildly
 * (more like an abuse of the ipc::session system's versus its intended usefulness); and 2, even then one could then
 * simply issue async_accept() R2 upon the success of async_accept() R1, if an ordering guarantee is really required.
 *
 * @internal
 * ### Implementation ###
 * Internally Session_server is really its `private` base: Session_server_impl parameterized
 * on the vanilla #Server_session_obj it produces.  See impl notes for that internal class template.
 * 100% of the work done by `*this` is done by that base.  Why does the base exist then?  Answer:
 * to implement Session_server variants with added capabilities, including the capability of producing
 * non-vanilla #Server_session variants.  For example: shm::classic::Session_server:
 *   - emits shm::classic::Server_session which is like #Server_session but with its own per-session boost.ipc.shm SHM
 *     pool-based arena;
 *   - and which also sets up a per-Client_app SHM arena in a boost.ipc.shm SHM pool, for each new distinct Client_app
 *     to start a session (it creates it if needed by name; and keeps handle open; it's an atomic operation).
 *
 * As for this class template, though, we add no such things, because we:
 *   - pass-through a no-op `per_app_setup_func()` that always yields falsy #Error_code;
 *   - emit vanilla Server_session (#Server_session_obj).
 * @endinternal
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Emitted `Server_session`s shall have the concrete typed based on this value for `S_MQ_TYPE_OR_NONE`.
 *         See #Server_session_obj.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Emitted `Server_session`s shall have the concrete typed based on this value for `S_TRANSMIT_NATIVE_HANDLES`.
 *         See #Server_session_obj.
 * @tparam Mdt_payload
 *         Emitted `Server_session`s shall have the concrete typed based on this value for `Mdt_payload`.
 *         See #Server_session_obj.  In addition the same type may be used for `mdt_from_cli_or_null` (and srv->cli
 *         counterpart) in async_accept().  (Recall that you can use a capnp-`union` internally
 *         for various purposes.)
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Session_server :
  private Session_server_impl // Attn!  Emit vanilla `Server_session`s (impl customization point: unused).
            <Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
             Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>
{
private:
  // Types.

  /// Short-hand for our base/core impl.
  using Impl = Session_server_impl
                 <Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
                  Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>;

public:
  // Types.

  /// Short-hand for the concrete `Server_session`-like type emitted by async_accept().
  using Server_session_obj = Server_session<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>;

  /// Short-hand for Session_mv::Mdt_reader_ptr.
  using Mdt_reader_ptr = typename Impl::Mdt_reader_ptr;

  /// Metadata builder type passed to `mdt_load_func()` in advanced async_accept() overload.
  using Mdt_builder = typename Server_session_obj::Mdt_builder;

  /// Short-hand for Session_mv::Channels.
  using Channels = typename Impl::Channels;

  // Constructors/destructor.

  /**
   * Constructor: immediately begins listening for incoming session-open attempts from allowed clients.
   * This will write to the CNS (PID file) and then listen on Unix domain
   * server socket whose abstract address is based off its contents (as explained in the class doc header).
   *
   * If either step fails (file error, address-bind error being the most likely culprits), an error is emitted
   * via normal Flow error semantics.  If this occurs, via the non-exception-throwing invocation style,
   * then you must not call async_accept(); or behavior is undefined (assertion may trip).
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param srv_app_ref
   *        Properties of this server application.  The address is copied; the object is not copied.
   * @param cli_app_master_set_ref
   *        The set of all known `Client_app`s.  The address is copied; the object is not copied.
   *        Technically, from our POV, it need only list the `Client_app`s whose names are
   *        in `srv_app_ref.m_allowed_client_apps`.  Refer to App doc header for best practices on
   *        maintaining this master list in practice.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        interprocess-mutex-related errors (probably from boost.interprocess) w/r/t writing the CNS (PID file);
   *        file-related system errors w/r/t writing the CNS (PID file) (see class doc header for background);
   *        errors emitted by transport::Native_socket_stream_acceptor ctor (see that ctor's doc header; but note
   *        that they comprise name-too-long and name-conflict errors which ipc::session specifically exists to
   *        avoid, so you need not worry about even the fact there's an abstract Unix domain socket address involved).
   */
  explicit Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                          const Client_app::Master_set& cli_app_master_set_ref,
                          Error_code* err_code = 0);

  /**
   * Destroys this acceptor which will stop listening in the background and cancel any pending
   * completion handlers by invoking them ASAP with
   * session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.  Subtlety:
   * expect session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, not
   * transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, regardless of the fact
   * that internally a transport::Native_socket_stream_acceptor is used.
   *
   * You must not call this from directly within a completion handler; else undefined behavior.
   *
   * Each pending completion handler will be called from an unspecified thread that is not the calling thread.
   * Any associated captured state for that handler will be freed shortly after the handler returns.
   *
   * We informally but very strongly recommend that your completion handler immediately return if the `Error_code`
   * passed to it is error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.  This is similar to what one should
   * do when using boost.asio and receiving the conceptually identical `operation_aborted` error code to an
   * `async_...()` completion handler.  In both cases, this condition means, "we have decided to shut this thing down,
   * so the completion handlers are simply being informed of this."
   */
  ~Session_server();

  // Methods.

  /**
   * Asynchronously awaits for an opposing Client_session to request session establishment and calls `on_done_func()`,
   * once the connection occurs and log-in exchange completes, or an error occurs, in the former case move-assigning an
   * almost-PEER-state Server_session object to the passed-in Server_session `*target_session`.
   * `on_done_func(Error_code())` is called on success.  `on_done_func(E)`, where `E` is a non-success
   * error code, is called otherwise.  In the latter case `*this` may continue operation, and further `async_accept()`s
   * may succeed.  See class doc header regarding error handling.
   *
   * Multiple async_accept() calls can be queued while no session-open is pending;
   * they will grab incoming connections in FIFO fashion as they arrive, but log-ins may race afterwards, resulting
   * in not-quite-FIFO emission via handler.  See class doc header regarding FIFO ordering.
   *
   * The aforementioned Server_session generated and move-assigned to `*target_session` on success shall
   * inherit `this->get_logger()` as its `->get_logger()`.
   *
   * `on_done_func()` shall be called from some unspecified thread, not the calling thread *and possibly concurrently
   * with other such completion handlers*.  Your implementation must be non-blocking.  Informally we recommend you
   * place the true on-event logic onto some task loop of your own; so ideally it would consist of essentially a
   * single `post(F)` statement of some kind.
   *
   * You must not call this from directly within a completion handler; else undefined behavior.
   *
   * #Error_code generated and passed to `on_done_func()`:
   * session::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER (destructor called, canceling all pending ops;
   * spiritually identical to `boost::asio::error::operation_aborted`),
   * other system codes most likely from `boost::asio::error` or `boost::system::errc` (but never
   * would-block), indicating the underlying transport::Native_socket_stream_acceptor is hosed for that specific reason,
   * those returned by transport::Native_socket_stream::remote_peer_process_credentials(),
   * those emitted by transport::struc::Channel::send(),
   * those emitted by transport::struc::Channel via on-error handler (most likely
   * transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE indicating graceful shutdown of opposing process
   * coincidentally during log-in procedure, prematurely ending session while it was starting),
   * error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN,
   * error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS,
   * error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH,
   * error::Code::S_INVALID_ARGUMENT (other side expected other async_accept() overload with
   * non-null `init_channels_by_cli_req` arg),
   * error::Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE (unable to acquire init-channel
   * resources -- as of this writing MQ-related ones).
   *
   * ### Error handling discussion ###
   * See class doc header regarding error handling, then come back here.
   * The last 3 specific #Error_code values listed can be considered, informally, individual misbehavior on
   * the part of the opposing client process.  While indicating potentially serious misconfiguration in the system,
   * surely to be investigated ASAP, it is conceivable to continue attempting further `async_accept()`s.
   *
   * Do note, though, that this is not a network client-server situation.  That is, probably, one should not expect
   * lots of -- or any -- such problems in a smoothly functioning IPC universe on a given production server machine.
   * E.g., there could be a security problem, or perhaps more likely there's a software
   * mismatch between client and server w/r/t their master App sets.  Your software deployment story may or may not
   * be designed to allow for this as a transient condition during software upgrades and such.
   *
   * @tparam Task_err
   *         Handler type matching signature of `flow::async::Task_asio_err`.
   * @param target_session
   *        Pointer to Server_session which shall be assigned an almost-PEER-state (open, requires
   *        Server_session::init_handlers() to enter PEER state) as `on_done_func()`
   *        is called.  Not touched on error.
   * @param on_done_func
   *        Completion handler.  See above.  The captured state in this function object shall be freed shortly upon
   *        its completed execution from the unspecified thread.
   */
  template<typename Task_err>
  void async_accept(Server_session_obj* target_session, Task_err&& on_done_func);

  /**
   * Identical to the simpler async_accept() overload but offers added advanced capabilities: metadata exchange;
   * initial-channel opening.  The other overload is identical to
   * `async_accept(target_session, nullptr, nullptr, nullptr, ...N/A..., NO_OP_FUNC, on_done_func)`
   * (where `NO_OP_FUNC()` no-ops) and requires the opposing `sync_connect()` to similarly not use the corresponding
   * features.
   *
   * The advanced capabilities complement/mirror the ones on Client_session_mv::sync_connect() (advanced overload);
   * see its doc header first.  Then come back here.  The same mechanisms underly the following; but due
   * to client-server asymmetry the API is somewhat different.
   *
   * ### Server->client metadata exchange ###
   * Server may wish to provide information in some way, but without a structured channel -- or any channel -- yet
   * available, this provides an opportunity to do so in a structured way with a one-off message available at
   * session open, together with the opposing Session object itself.
   *
   * The metadata payload you may wish to load to emit to the opposing `Client_session::sync_connect()` may
   * depend on the following information available *during* the session-open procedure, namely upon receiving
   * (internally) the log-in request from the client side:
   *   - the Client_app that wishes to open the session;
   *   - how many init-channels the client is requesting be opened (see below);
   *   - the client->Server metadata (see below).
   *
   * Thus supply `Mdt_load_func mdt_load_func` arg which takes all 3 of these data, plus a blank
   * capnp `Mdt_payload` structure to fill; and loads the latter as desired.
   *
   * To omit using this feature do nothing in `mdt_load_func()`.  (The other async_accept() overload provides such
   * a no-op function.)
   *
   * ### Client->server metadata exchange ###
   * This is the reverse of the above.  Whatever the opposing client chose to supply as client->server metadata
   * shall be deserializable at `*mdt_from_cli_or_null` once (and if) `on_done_func(Error_code())` (successful
   * accept) fires.  If `mdt_from_cli_or_null` is null, the cli->srv metadata shall be ignored.
   *
   * ### Init-channels by server request ###
   * Once the session is open, open_channel() and the on-passive-open handler may be used to open channels at will.
   * In many use cases, however, a certain number of channels is required immediately before work can really begin
   * (and, frequently, no further channels are even needed).  For convenience (to avoid asynchrony/boiler-plate)
   * the init-channels feature will pre-open a requested # of channels making them available right away, together
   * with the freshly-open session -- to both sides.
   *
   * The server may request 0 or more init-channels.  They shall be opened and placed into
   * `*init_channels_by_srv_req`.  The number of channels requested may depend on the 3 piece of info outlined
   * above in "Server->client metadata exchange."  Thus supply `N_init_channels_by_srv_req_func
   * n_init_channels_by_srv_req_func` arg which takes all 3 of these data and returns the `size_t` channel
   * count (possibly 0).  The resulting channels shall be loaded into `*init_channels_by_srv_req`
   * before successful `on_done_func()` execution.
   *
   * If and only if `n_init_channels_by_srv_req_func()` would always return 0, you may provide a
   * null `init_channels_by_srv_req`.  `n_init_channels_by_srv_req_func` will then be ignored.
   *
   * ### Init-channels by client request ###
   * This is the reverse of the above.  The opposing side shall request 0 or more init-channels-by-client-request;
   * that number of channels shall be opened; and they will be placed into `*init_channels_by_cli_req` which shall
   * be `->resize()`d accordingly, once (and if) `on_done_func(Error_code())` (successful
   * connect) fires.
   *
   * `init_channels_by_srv_cli` being null is allowed, but only if the opposing server requests 0
   * init-channels-by-server-request.  Otherwise an error shall be emitted (see below).
   *
   * @tparam Task_err
   *         See other async_accept() overload.
   * @tparam N_init_channels_by_srv_req_func
   *         See above: function type matching signature
   *         `size_t F(const Client_app&, size_t n_init_channels_by_cli_req, Mdt_reader_ptr&& mdt_from_cli)`.
   * @tparam Mdt_load_func
   *         See above: function type matching signature
   *         `void F(const Client_app&, size_t n_init_channels_by_cli_req, Mdt_reader_ptr&& mdt_from_cli,
   *                 Mdt_builder* mdt_from_srv)`.
   * @param target_session
   *        See other async_accept() overload.
   * @param init_channels_by_srv_req
   *        See above: null or pointer to container of `Channel_obj` which shall be `.clear()`ed and replaced
   *        by a container of PEER-state `Channel_obj` on success the number being determined by
   *        `n_init_channels_by_srv_req_func()`.  null is treated as-if `n_init_channels_by_srv_req_func() == 0`
   *        for any input.
   * @param mdt_from_cli_or_null
   *        See above: null or pointer to `Reader` of metadata which shall be set for access on success.
   * @param init_channels_by_cli_req
   *        See above: null or pointer to container of `Channel_obj` which shall be `.clear()`ed and replaced
   *        by a container of PEER-state `Channel_obj` on success the number being specified by the opposing
   *        (client) side.  The number may be zero.  null is allowed if and only if the number is zero;
   *        otherwise error::Code::S_INVALID_ARGUMENT is emitted.
   * @param n_init_channels_by_srv_req_func
   *        See `N_init_channels_by_srv_req_func`.  Ignored on null `init_channels_by_srv_req`.
   * @param mdt_load_func
   *        See `Mdt_load_func`.
   * @param on_done_func
   *        See other async_accept() overload.  Note the above target (pointer) args are touched
   *        only if falsy #Error_code is passed to this handler.
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

  // XXX
  template<typename... Accept_args>
  void sync_accept(util::Fine_duration timeout, Error_code* err_code, Accept_args&&... accept_args);
  template<typename... Accept_args>
  void sync_accept(Error_code* err_code, Accept_args&&... accept_args);

  /**
   * Prints string representation to the given `ostream`.
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

  // The LOG_*() macros don't see Log_context::get_log*() from base otherwise....
  using Impl::get_logger;
  using Impl::get_log_component;
}; // class Session_server

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SESSION_SERVER \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SESSION_SERVER \
  Session_server<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

TEMPLATE_SESSION_SERVER
CLASS_SESSION_SERVER::Session_server(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref_arg,
                                     const Client_app::Master_set& cli_app_master_set_ref,
                                     Error_code* err_code) :
  Impl(logger_ptr, this, srv_app_ref_arg, cli_app_master_set_ref, err_code,
       [](const Client_app&) -> Error_code { return Error_code(); }) // Impl customization point: unused.
{
  // OK then.
}

TEMPLATE_SESSION_SERVER
CLASS_SESSION_SERVER::~Session_server() = default; // It's declared at all just to document it.  Forward to base.

TEMPLATE_SESSION_SERVER
template<typename Task_err>
void CLASS_SESSION_SERVER::async_accept(Server_session_obj* target_session, Task_err&& on_done_func)
{
  // As advertised this overload always means:

  auto ignored_func = [](auto&&, auto&&, auto&&) -> size_t { return 0; };
  auto no_op_func = [](auto&&, auto&&, auto&&, auto&&) {};

  async_accept(target_session, nullptr, nullptr, nullptr, std::move(ignored_func), std::move(no_op_func),
               std::move(on_done_func));

  /* @todo shm::classic::Session_server::async_accept() counterpart is a copy-paste of the above.
   * shm::arena_lend::jemalloc::Session_erver::async_accept() is too.
   * Maybe the design can be amended for greater code reuse/maintainability?  This isn't *too* bad but....
   *
   * Anyway, for now, if you change this then change the aforementioned forwarding code in the other
   * public `Session_server`s. */
}

TEMPLATE_SESSION_SERVER
template<typename Task_err,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_SESSION_SERVER::async_accept(Server_session_obj* target_session,
                                        Channels* init_channels_by_srv_req,
                                        Mdt_reader_ptr* mdt_from_cli_or_null,
                                        Channels* init_channels_by_cli_req,
                                        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                                        Mdt_load_func&& mdt_load_func,
                                        Task_err&& on_done_func)
{
  Impl::async_accept(target_session, init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                     std::move(n_init_channels_by_srv_req_func), std::move(mdt_load_func), std::move(on_done_func));
}

TEMPLATE_SESSION_SERVER
template<typename... Accept_args>
void CLASS_SESSION_SERVER::sync_accept(util::Fine_duration timeout, Error_code* err_code, Accept_args&&... accept_args)
{
  Impl::sync_accept(timeout, err_code, std::forward<Accept_args>(accept_args)...);
}

TEMPLATE_SESSION_SERVER
template<typename... Accept_args>
void CLASS_SESSION_SERVER::sync_accept(Error_code* err_code, Accept_args&&... accept_args)
{
  sync_accept(util::Fine_duration::max(), err_code, std::forward<Accept_args>(accept_args)...);
}

TEMPLATE_SESSION_SERVER
void CLASS_SESSION_SERVER::to_ostream(std::ostream* os) const
{
  Impl::to_ostream(os);
}

TEMPLATE_SESSION_SERVER
std::ostream& operator<<(std::ostream& os, const CLASS_SESSION_SERVER& val)
{
  val.to_ostream(&os);
  return os;
}

#undef CLASS_SESSION_SERVER
#undef TEMPLATE_SESSION_SERVER

} // namespace ipc::session
