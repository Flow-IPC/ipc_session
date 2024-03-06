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

#include "ipc/session/detail/session_base.hpp"
#include "ipc/session/error.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/util/detail/util.hpp"
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/move/make_unique.hpp>
#include <cstddef>

namespace ipc::session
{

/**
 * Internal, non-movable pImpl-lite implementation of Client_session_mv class template.
 * In and of itself it would have been directly and publicly usable; however Client_session_mv adds move semantics.
 *
 * @see All discussion of the public API is in Client_session_mv doc header; that class template forwards to this
 *      one.  All discussion of pImpl-lite-related notions is also there.  See that doc header first please.  Then come
 *      back here.
 *
 * Impl design
 * -----------
 * Thread U represents all threads other than thread W: since the relevant methods are to be called by the user
 * sans concurrency, those threads as a collection can be thought of as one thread.
 *
 * Thread W is the async worker thread where most work is done; this helps us entirely avoid mutexes.
 * Both async_connect() (from sync_connect()) and open_channel() are fairly infrequently called, so it's not necessary
 * to avoid latency by doing work concurrently in threads U and W.  So we keep it simple by posting most stuff
 * onto thread W.
 *
 * There are two distinct states (other than NULL): CONNECTING (async_connect() outstanding) and PEER (it succeeded:
 * `*this` is now a Session in PEER state per concept).  (As of this writing async_connect() is `private` and
 * invoked by `public` sync_connect(); more on that jsut below.)
 *
 * ### CONNECTING state impl ###
 * Once async_connect() is issued we must do the following:
 *   -# Synchronously find out the full `Shared_name` where the opposing Session_server is allegedly listening,
 *      which is computable by a #Base helper.  So that it can be computed we need to find out
 *      Session_base::srv_namespace() which we immediately do by checking the expected file that should have been
 *      written by Session_server.
 *   -# Issue the Native_socket_stream::sync_connect() which shall synchronously/non-blockingly connect to that
 *      abstract-address.
 *   -# Once that's done, go back to NULL on failure; or continue as follows on success: The Native_socket_stream,
 *      now in PEER state (connected), is upgraded to (wrapped by) a transport::Channel which is in turn upgraded to
 *      a transport::struc::Channel Client_session_impl::m_master_channel.
 *      This is the session master channel, in log-in phase (as client)
 *      (details of that API are in transport::struc::Channel docs).  So we immediately issue the next
 *      async step: sending the log-in request over `m_master_channel` and async-await log-in response.
 *   -# Once that's done, go back to NULL on failure; or else yay: we're now in PEER state...
 *   -# ...almost.  If the advanced form of sync_connect() was the one invoked, there's an additional exchange of
 *      info about init-channels, wherein the opposing Session_server sends us info about the pre-opened init-channels.
 *      If so, PEER state is reached upon receipt of this; otherwise immediately.
 *
 * See session_master_channel.capnp for the schema for `m_master_channel`.  Reading that gives a nice overview of
 * the protocol involved, both in CONNECTING (after the socket is connected) and in PEER.  Also notable is that
 * this setup allows us to immediately "eat our own dog food" by using a `struc::Channel` to enable the opening
 * of any further channels (structured or otherwise).
 *
 * ### CONNECTING state and asynchronicity ###
 * As written, sync_connect() -- as a black box -- is synchronous and non-blocking.  It is written trivially
 * as invoking async_connect() (which is `private`); giving it a simple completion handler that fulfills a
 * `promise`; sync_connect() awaits this fulfillment in the user's calling thread and then returns.  This raises
 * two key questions.
 *
 * One, how is this even possible -- if there's (even internally) an async step, how can sync_connect() be
 * synchronous yet non-blocking?  Answer: First we quote the Client_session_mv public doc header which explains
 * why there is only `sync_connect()` but no `async_connect()`:
 * "Without networking, the other side (Session_server) either exists/is listening; or no.
 * Connecting is a synchronous, non-blocking operation; so an `async_connect()` API in this context only makes
 * life harder for the user.  (However, there are some serious plans to add a networking-capable counterpart
 * (probably via TCP at least) to `Native_socket_stream` and therefore Client_session_mv; such a networky-client-session
 * class will almost certainly have an `async_connect()`, while its `sync_connect()` will probably become potentially
 * blocking.)"  So that is why it works: in a local setting, the socket-connect is synchronous/fast; and upon
 * connecting the necessary log-in/init-channel exchange is also quick.
 *
 * Two: Why is it written like this?  Wouldn't the internal code be simpler, if it lacked asynchronicity?
 * The answer is two-fold:
 *   - The mundane reason as of this writing is there is no transport::struc::Channel `sync_expect_msgs()`,
 *     a hypothetical method that'd await N incoming messages.  That's a to-do maybe: provide
 *     `sync_expect_msg[s]()` which would be to `expect_msg[s]()` what `sync_request()` is to `async_request()`.
 *     Then the init-channel phase can be written in synchronous form.
 *     - That said, the other steps listed above are already writable in synchronous form:
 *       transport::Native_socket_stream::sync_connect() we already use; the log-in `.async_request()` can be
 *       trivially replaced with `.sync_request()`.
 *   - But the true, deeper reason has to do with the above public-facing quote: When/if we network-enable
 *     some form of `*this`, all 2-3 quick/synchronous steps described will potentially block; and we'll need
 *     to make async_connect() a public API.  At that point writing sync_connect() in terms of async_connect()
 *     will pay dividends.  The trade-off is the existing code is somewhat more complex in the meantime.
 *     - Detail: Since there is no public `Native_socket_stream::async_connect()` (as it, too, is not network-enabled),
 *       we do use the simpler-to-use `Native_socket_stream::sync_connect()`.  Once network-enabled, that guy
 *       will become potentially blocking; so we'd need to start using the speculated public-API `async_connect()`.
 *
 * A related aspect is that `on_done_func` completion handler we pass to async_connect() currently *will* fire
 * before sync_connect() returns; so there's no contract for the dtor to fire it with operation-aborted error code,
 * and we can just `assert()` on this in dtor and not worry about it further.  Yet if async_connect() becomes public,
 * then there's no longer that guarantee, and we'll need to fire it as needed from dtor.
 *
 * In the code itself we sometimes refer to the above discussion to hopefully make maintenance/future development
 * easier.
 *
 * @note If some form of `*this` becomes network-enabled, open_channel() too will need an async form most likely,
 *       while existing open_channel() would become potentially-blocking.
 *
 * ### Protocol negotiation ###
 * First please see transport::struc::sync_io::Channel doc header section of the same name.  (There is involved
 * discussion there which we definitely don't want to repeat.)  Then come back here.
 *
 * How we're similar: `Channel` decided to divide the protocols into 2: one in struc::sync_io::Channel code itself,
 * and the rest in whatever extra stuff might be involved in the (likely SHM-related, optionally used)
 * non-vanilla transport::struc::Struct_builder and transport::struc::Struct_reader concept impls (namely
 * most likely transport::struc::shm::Builder and transport::struc::shm::Reader).  So essentially there's the
 * `ipc_transport_structured` part, and the (other) part (likely but not necessarily from `ipc_shm`).  We do
 * something vaguely similar conceptually:
 *   - There is the basic ipc::session protocol, including the `LogInReq/LogInRsp/OpenChannel*` messages.  This
 *     is from ipc::session proper (`ipc_session` module/library).
 *   - There is the optionally-used protocol woven into it activated by the fact the user chose to use
 *     ipc::session::shm (or maybe even their own) `Session` hierarchy.  (As of this writing, ipc::session::shm
 *     contains two hierarchies, depending on the SHM-provider of choice: SHM-classic `shm::classic` or
 *     SHM-jemalloc `shm::arena_lend::jemalloc`.)
 *
 * So we, too, maintain two `Protocol_negotiator`s: the main one and the "aux" one for those two bullet points
 * respectively.  As of this writing there is only version 1 of everything; but if this changes for one of the
 * "aux" `Session` hierarchies -- e.g., SHM-jemalloc's internally used ipc_session_message.capnp channel/schema --
 * then that guy's "aux" protocol-version can be bumped up.  (In the case of ipc_session_message.capnp specifically,
 * and I say this only for exposition, that code can -- if it wants -- do its own protocol negotiation along its
 * own channel; but by leveraging the "aux" `Protocol_negotiator` version infrastructure, they do not have to.
 * Good for perf and, in this case more importantly, good for keeping code complexity down.)
 *
 * How we're different: The mechanics of protocol-version exchange are simpler for us, because we have a built-in
 * SYN/SYN-ACK like exchange at the start (log-in-request, log-in-response) via the session master channel.
 * So we piggy-back (piggy-front?) the protocol-negotiation info onto those 2 messages which exist anyway.
 * You can see that now by looking at session_master_channel.capnp and noting how `ProtocolNegotiation` fields
 * sit up-top in `LogInReq` and `LogInRsp`.  As Protocol_version doc header discussion suggests, it is important
 * to verify the `ProtocolNegotiation` fields before interpreting any other part of the incoming `LogInReq` and
 * `LogInRsp` by server and client respectively (and in that chronological order, as guaranteed by the handshake
 * setup).
 *
 * (In contrast to this: struc::sync_io::Channel does not itself have a mandatory built-in handshake phase.  Hence
 * each side has to send a special `ProtocolNegotiation` message ASAP and expect one -- in each pipe of up-to-2 --
 * to come in as well.  No handshake/client/server => the two exchanges are full-duplex, that is mutually
 * independent, in nature.)
 *
 * If you're keeping score: as of this writing, the most complex/full-featured setup for a session is
 * the SHM-jemalloc session setup.  Excluding any structured channels opened within the session, the following
 * transport::Protocol_negotiator negotiations shall occur:
 *   - Session master channel `Native_socket_stream` low-level protocol (1 `Protocol_negotiator` per side).
 *     Full-duplex exchange.
 *   - Session master channel `struc::Channel` protocol (2 `Protocol_negotiator`s per side, "aux" one unused
 *     because the session master channel is a vanilla, non-SHM-backed channel).
 *     Full-duplex exchange.
 *   - Session master channel vanilla session protocol (1 `Protocol_negotiator` per side).
 *     Handshake exchange.
 *   - Session master channel SHM-jemalloc session protocol (1 `Protocol_negotiator` per side).
 *     Handshake exchange (same one as in the previous bullet point).
 *
 * Unrelated to sessions, consider the most complex/full-featured setup for a user `transport::struc::Channel` operating
 * over a 2-pipe `transport::Channel` (1 pipe for blobs, 1 pipe for `Native_handle`s):
 *   - User channel `Native_socket_stream` low-level protocol (1 `Protocol_negotiator` per side).
 *     Full-duplex exchange (pipe 1).
 *   - User channel `Blob_stream_mq_*er` low-level protocol (1 `Protocol_negotiator` per side).
 *     Full-duplex exchange (pipe 2, same layer as preceding bullet point, different transport type).
 *   - Session master channel `struc::Channel` protocol (2 `Protocol_negotiator`s per side; main one for the vanilla
 *     layer, "aux" one for the SHM layer).
 *     Full-duplex exchange.
 *
 * ### PEER state impl ###
 * Here our algorithm is complementary to the PEER state algorithm in Server_session_impl.  Namely we expect
 * passive-opens via an appropriate transport::struc::Channel::expect_msgs(); and we allow active-opens via
 * open_channel().  The details of how these work is best understood just by reading that code inline.
 *
 * open_channel(), similarly to Server_session_impl::open_channel(), has one interesting quirk which is:
 * To the user it is presented as synchronous and blocking with the aim of being non-blockingly fast, as long
 * as the other side is in PEER state.  It uses transport::struc::Channel::sync_request() with a generous timeout, while
 * knowing that if both sides in PEER state in practice the call will complete rather swiftly; the generous
 * timeout aimed making it clear that open_channel() is failing *not* due to some overly tight internal timeout
 * but more likely due to some application problem such as not calling Server_session::init_handlers() quickly
 * enough.
 *
 * Once PEER state is reached, the Session-concept error emission semantics come into play.  See its doc header.
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See #Client_session counterpart.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See #Client_session counterpart.
 * @tparam Mdt_payload
 *         See #Client_session counterpart.
 * @tparam S_SHM_TYPE_OR_NONE
 *         Identical to opposing Server_session_impl counterpart.
 * @tparam S_GRACEFUL_FINISH_REQUIRED_V
 *         `true` if and only if Session_base::Graceful_finisher must be used.
 *         See its doc header for explanation when that would be the case.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload,
         schema::ShmType S_SHM_TYPE_OR_NONE, bool S_GRACEFUL_FINISH_REQUIRED_V>
class Client_session_impl :
  public Session_base<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
  public flow::log::Log_context
{
public:
  // Types.

  /// Short-hand for base type.
  using Base = Session_base<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>;

  /// Short-hand for Session_base super-class.
  using Session_base_obj = Base;

  /// See Client_session_mv counterpart.
  using Channel_obj = typename Base::Channel_obj;

  /// See Session_mv counterpart.
  using Channels = typename Base::Channels;

  /// See Client_session_mv counterpart.
  using Mdt_payload_obj = typename Base::Mdt_payload_obj;

  /// See Client_session_mv counterpart.
  using Mdt_reader_ptr = typename Base::Mdt_reader_ptr;

  /// See Client_session_mv counterpart.
  using Mdt_builder = typename Base::Mdt_builder;

  /// See Client_session_mv counterpart.
  using Mdt_builder_ptr = typename Base::Mdt_builder_ptr;

  /// See Session_mv counterpart.
  using Structured_msg_builder_config = typename Base::Structured_msg_builder_config;

  /// See Session_mv counterpart.
  using Structured_msg_reader_config = typename Base::Structured_msg_reader_config;

  // Constants.

  static_assert(S_SHM_TYPE_OR_NONE != schema::ShmType::END_SENTINEL,
                "Do not set it to sentinel enum value which is only for counting # of possible values and such.");

  /// See Session_mv counterpart.
  static constexpr schema::ShmType S_SHM_TYPE = S_SHM_TYPE_OR_NONE;

  /// See Session_mv counterpart.
  static constexpr bool S_SHM_ENABLED = S_SHM_TYPE != schema::ShmType::NONE;

  /// See Client_session_mv counterpart.
  static constexpr bool S_MQS_ENABLED = Base::S_MQS_ENABLED;

  /// See Client_session_mv counterpart.
  static constexpr bool S_SOCKET_STREAM_ENABLED = Base::S_SOCKET_STREAM_ENABLED;

  /// Short-hand for template parameter knob `S_GRACEFUL_FINISH_REQUIRED_V`: see class template doc header.
  static constexpr bool S_GRACEFUL_FINISH_REQUIRED = S_GRACEFUL_FINISH_REQUIRED_V;

  // Constructors/destructor.

  /**
   * See Client_session_mv counterpart.
   *
   * @param logger_ptr
   *        See Client_session_mv counterpart.
   * @param cli_app_ref
   *        See Client_session_mv counterpart.
   * @param srv_app_ref
   *        See Client_session_mv counterpart.
   * @param on_err_func
   *        See Client_session_mv counterpart.
   * @param on_passive_open_channel_func
   *        See Client_session_mv counterpart.
   */
  template<typename On_passive_open_channel_handler, typename Task_err>
  explicit Client_session_impl(flow::log::Logger* logger_ptr,
                               const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                               Task_err&& on_err_func,
                               On_passive_open_channel_handler&& on_passive_open_channel_func);

  /**
   * See Client_session_mv counterpart.
   *
   * @param logger_ptr
   *        See Client_session_mv counterpart.
   * @param cli_app_ref
   *        See Client_session_mv counterpart.
   * @param srv_app_ref
   *        See Client_session_mv counterpart.
   * @param on_err_func
   *        See Client_session_mv counterpart.
   */
  template<typename Task_err>
  explicit Client_session_impl(flow::log::Logger* logger_ptr,
                               const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                               Task_err&& on_err_func);

  /// See Client_session_mv counterpart.
  ~Client_session_impl();

  // Methods.

  /**
   * See Client_session_mv counterpart.  Reminder: it can be used not only in PEER state (for open_channel()) but
   * also NULL state (for async_connect()).
   *
   * @return See Client_session_mv counterpart.
   */
  Mdt_builder_ptr mdt_builder() const;

  /**
   * See Client_session_mv counterpart.
   *
   * @param err_code
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  bool sync_connect(Error_code* err_code);

  /**
   * See Client_session_mv counterpart.
   *
   * @param mdt
   *        See Client_session_mv counterpart.
   * @param init_channels_by_cli_req_pre_sized
   *        See Client_session_mv counterpart.
   * @param mdt_from_srv_or_null
   *        See Client_session_mv counterpart.
   * @param init_channels_by_srv_req
   *        See Client_session_mv counterpart.
   * @param err_code
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  bool sync_connect(const Mdt_builder_ptr& mdt,
                    Channels* init_channels_by_cli_req_pre_sized,
                    Mdt_reader_ptr* mdt_from_srv_or_null,
                    Channels* init_channels_by_srv_req,
                    Error_code* err_code);

  /**
   * See Client_session_mv counterpart.
   *
   * @param target_channel
   *        See Client_session_mv counterpart.
   * @param mdt
   *        See Client_session_mv counterpart.
   * @param err_code
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  bool open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt, Error_code* err_code);

  /**
   * See Client_session_mv counterpart.
   *
   * @param target_channel
   *        See Client_session_mv counterpart.
   * @param err_code
   *        See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  bool open_channel(Channel_obj* target_channel, Error_code* err_code);

  /**
   * See Client_session_mv counterpart.
   * @return See Client_session_mv counterpart.
   */
  const Session_token& session_token() const;

protected:
  // Types.

  /// See Session_base.
  using Master_structured_channel = typename Base::Master_structured_channel;

  // Methods.

  /**
   * Utility for `*this` and sub-classes: Implements sync_connect() given a functor that invokes
   * the calling class's (including Client_session_impl) async_connect() with the args passed to the calling
   * sync_connect().
   *
   * This allows sub-classes to reuse our straightforward `promise`-based implementation of sync_connect()
   * in terms of async_connect().  Otherwise they'd need to copy/paste that stuff or something.
   * E.g., see shm::classic::Client_session_impl and/or shm::arena_lend::jemalloc::Client_session_impl.
   * If sub-classes were not an issue, sync_connect_impl() would not be needed; we'd just call our
   * async_connect() directly.
   *
   * @param err_code
   *        See sync_connect().
   * @param async_connect_impl_func
   *        Non-null pointer to a function that (1) calls `async_connect()`-like method, forwarding to it
   *        the args to the calling `sync_connect()`-like method, except (2) the last arg is to be a forward
   *        of the `on_done_func`-signature-matching arg to `async_connect_impl_func`.
   * @return See sync_connect().
   */
  bool sync_connect_impl(Error_code* err_code, Function<bool (flow::async::Task_asio_err&&)>* async_connect_impl_func);

  /**
   * Core implementation of sync_connect().
   * See "CONNECTING state and asynchronicity" in class doc header for key background.
   * 
   * @return See sync_conect().
   * @param mdt
   *        See sync_conect().
   * @param init_channels_by_cli_req_pre_sized
   *        See sync_conect().
   * @param mdt_from_srv_or_null
   *        See sync_conect().
   * @param init_channels_by_srv_req
   *        See sync_conect().
   * @param on_done_func
   *        What to invoke in thread W on completion of the async-connect.  As of this writing this is supplied
   *        by sync_connect_impl() internal code.
   */
  template<typename Task_err>
  bool async_connect(const Mdt_builder_ptr& mdt,
                     Channels* init_channels_by_cli_req_pre_sized,
                     Mdt_reader_ptr* mdt_from_srv_or_null,
                     Channels* init_channels_by_srv_req,
                     Task_err&& on_done_func);

  /**
   * Utility for sub-classes: executed from async_connect()'s `on_done_func(Error_code())` (i.e., directly
   * from handler invoked on success) -- hence from thread W -- this instead goes back to NULL state,
   * essentially indicating "never mind -- this successful async_connect() actually failed."
   *
   * This... um... highly specific utility is intended for overriding async_connect() impl that performs
   * *synchronous* post-processing in its own `on_done_func()` wrapper from sub-class's async_connect()
   * that calls this class's async_connect().
   */
  void cancel_peer_state_to_null();

  /**
   * Utility for sub-classes: executed from async_connect()'s `on_done_func(Error_code())` (i.e., directly
   * from handler invoked on success) -- hence from thread W -- this instead goes back to CONNECTING state,
   * essentially indicating "yes, the vanilla async_connect() succeeded, but the true user-called async_connect()
   * that piggybacked on the vanilla one will now do some more async stuff before reaching PEER state (or NULL on
   * error)."
   *
   * As `on_done_func` was already invoked (and thus forgotten), the caller must supply a new
   * one to execute once either PEER or NULL state is reached again.  Since `*this` has completed its vanilla
   * async_connect() work already, the responsibility for reaching one of those 2 states is now on the caller.
   * Therefore whatever async processing follows our invocation is required to call
   * complete_async_connect_after_canceling_peer_state().  That said, if a master-session-channel error occurs before
   * that has a chance to be called, then:
   *   - `*this` will enter NULL state itself.
   *   - `*this` will invoke the handler supplied here to cancel_peer_state_to_connecting().
   *
   * The session master channel is returned, so that the sub-class can take over operation as needed until
   * either complete_async_connect_after_canceling_peer_state() or that channel becoming hosed (either way
   * on_done_func() will be invoked).  This avoids providing unrestricted `protected` access to it all times.
   * See also async_worker().
   *
   * @tparam Task_err
   *         See async_connect().
   * @param on_done_func
   *        See async_connect().
   * @return See above.
   */
  template<typename Task_err>
  Master_structured_channel* cancel_peer_state_to_connecting(Task_err&& on_done_func);

  /**
   * Utility for sub-classes: to be invoked, only from thread W and asynchronously at that, after
   * cancel_peer_state_to_connecting(), this indicates the completion of the sub-class's additional async-connect
   * steps, setting state to either NULL or PEER.  Invoke only in CONNECTING state; else undefined behavior
   * (assertion may trip).
   *
   * The handler given to cancel_peer_state_to_connecting() will execute.
   *
   * Take care to check Session_base::hosed() as needed before calling this; if hosed then do not call.
   *
   * @param err_code_or_success
   *        State will proceed to PEER if falsy else to NULL; in the latter case the given code shall be passed to
   *        handler as the reason for failure to ultimately async-connect.
   */
  void complete_async_connect_after_canceling_peer_state(const Error_code& err_code_or_success);

  /**
   * Utility for sub-classes: provides ability to schedule or post tasks onto thread W.
   * @return See above.
   */
  flow::async::Single_thread_task_loop* async_worker();

  /**
   * Utility for sub-classes: provides ability to *immutably query* the session master channel, particularly after
   * our async_accept_log_in() succeeds, but before the sub-classed wrapper of its on-done handler succeeds.
   * For example one can harmlessly query transport::struc::Channel::session_token().
   *
   * `protected` but `const` access does not much bother me (ygoldfel) stylistically.
   *
   * @return See above.
   */
  const Master_structured_channel& master_channel_const() const;

  /**
   * Synchronously stops async_worker() loop, the post-condition being that thread W has been joined; no tasks
   * `post()`ed onto it by `*this` or subclass shall execute after this returns.
   *   - If `*this` is not being subclassed, or if it never posts onto async_worker(): Our own dtor shall call
   *     this first-thing; no need for subclass to worry.
   *   - If `*this` is being subclassed, and it does post onto async_worker(): The terminal subclass's dtor must
   *     call this ~first-thing (our own dtor, once it is soon called, shall know not to re-execute the same thing).
   *
   * Call at most once; otherwise undefined behavior (assertion may trip).
   *
   * This exists to avoid a race, however unlikely, between a subclass's asynchronous accept-log-in code posted
   * onto thread W, and that same class's dtor being invoked by the user in thread U.  There is a short time period,
   * when thread W (#m_async_worker) is active -- `*this` is still intact -- but the subclass's members are being
   * destroyed by its dtor.  In that case dtor_async_worker_stop() would be called by the subclass dtor to put an
   * end to async shenanigans in thread W, so it can continue destroying self in peace.  When there is no subclass,
   * our own dtor does so.
   */
  void dtor_async_worker_stop();

private:
  // Types.

  /// See Session_base.
  using Master_structured_channel_ptr = typename Base::Master_structured_channel_ptr;

  /// See Session_base.
  using Master_structured_channel_observer = typename Base::Master_structured_channel_observer;

  /// See Session_base.
  using Persistent_mq_handle_from_cfg = typename Base::Persistent_mq_handle_from_cfg;

  /// See Session_base.
  using On_passive_open_channel_func = typename Base::On_passive_open_channel_func;

  /// Overall state of a Client_session_impl.
  enum class State
  {
    /**
     * Not a peer.  Initial state; goes to CONNECTING (via async_connect()).
     * Only async_connect() is possible in this state (other public mutators return immediately).
     */
    S_NULL,

    /**
     * Not a peer but async_connect() in progress to try to make it a peer.  Barring moves-from/moves-to:
     * Entry from NULL; goes to PEER or NULL.
     */
    S_CONNECTING,

    /**
     * Is or was a connected peer.  Entry from CONNECTING; does not transition to any other state
     * (once a PEER, always a PEER).  `*_connect()` is not possible in this state (it returns immediately); all
     * other mutating public methods are possible.
     */
    S_PEER
  }; // enum class State

  /**
   * An open-channel/log-in request out-message.  Together with that is stored a `Builder` into its metadata
   * sub-field.
   *
   * ### Rationale ###
   * This explanation is written for open_channel() case, but the same applies for the log-in request and
   * async_connect().
   *
   * Why store the `Builder`, if it can always be obtained from #m_req_msg?  Answer: On one hand,
   * mdt_builder() needs to return that `Builder`, so that the user can set the metadata.  However,
   * open_channel() aims to then stuff the underlying bits into the open-channel request out-message which is
   * is our internal impl detail, unknown to the user.  We could return a stand-alone just-the-metadata structure
   * and `Builder` to it, but (1) then we'd have to transfer that over into the out-message -- slow, possibly
   * painful (involving capnp orphans), and (2) it has to be backed by a `MessageBuilder`.  So we'd have to
   * set up a `MallocMessageBuilder` or something just for this thing -- only to end up painfully copying over
   * data from one serialization to another.  Instead we maintain zero-copy and avoid these issues:
   *   - Set up the open-channel request out-message directly in mdt_builder().  (Its ref-count shall
   *     always be 1.)
   *   - Point a `Builder` to the metadata sub-field.
   *   - Wrap both things in this `struct` and wrap that in a `shared_ptr`.
   *   - Lastly have mdt_builder() return an *alias* `shared_ptr` that (to the user) looks like it's
   *     to just the `Builder` (all they care about), but it's actually in the `shared_ptr` group of the real
   *     ref-counted datum, this `struct`.
   *   - open_channel() receives the alias, which points to the `Builder`.  Since the `Builder` is the first
   *     member of this `struct`, its raw address (by C++ aliasing rules) equals that of the whole `struct`.
   *     Hence open_channel() can get to the `struct` and thus to the out-message member #m_req_msg.  It can
   *     then transport::struc::Channel::send() that.
   */
  struct Master_channel_req
  {
    // Data.

    /**
     * The `Builder` `m_req_msg->body_root()->getMetadata()`.
     * @warning As of this writing this *must* be the first member.  Otherwise one would need to use something like
     *          `offsetof` to be able to get from `&m_mdt_builder` (what mdt_builder() returns)
     *          to `this` and hence to #m_req_msg to `send()`.
     */
    typename Mdt_builder_ptr::element_type m_mdt_builder;

    /// The open-channel-request-to-server out-message for the session master channel (in subsequent open_channel()).
    typename Master_structured_channel::Msg_out m_req_msg;
  };

  /**
   * Short-hand for ref-counted pointer to Master_channel_req.
   * @see Master_channel_req doc header for info on the true nature of such `shared_ptr` as returned by us.
   */
  using Master_channel_req_ptr = boost::shared_ptr<Master_channel_req>;

  // Constructors.

  /**
   * Delegated-to ctor that implements both non-default `public` ctors.
   *
   * @param logger_ptr
   *        See public ctors.
   * @param cli_app_ref
   *        See public ctors.
   * @param srv_app_ref
   *        See public ctors.
   * @param on_err_func
   *        See public ctors.  This is the concretely-typed version of that arg.
   * @param on_passive_open_channel_func_or_empty_arg
   *        See public ctors.  This is the concretely-typed version of that arg; `.empty()` if the version
   *        without this arg was used.
   * @param tag
   *        Ctor-selecting tag.
   */
  explicit Client_session_impl(flow::log::Logger* logger_ptr,
                               const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                               flow::async::Task_asio_err&& on_err_func,
                               On_passive_open_channel_func&& on_passive_open_channel_func_or_empty_arg,
                               std::nullptr_t tag);

  // Methods.

  /**
   * In thread W, handler for #m_master_channel indicating incoming-direction channel-hosing error.
   * It is possible that #m_master_channel has been `.reset()` in the meantime, by seeing log-in failure
   * in on_master_channel_log_in_rsp(), and no longer exists.  This is tested via the observer arg.
   *
   * @param master_channel_observer
   *        `weak_ptr` observer of #m_master_channel at the time it was constructed.
   *        `master_channel_observer.lock()` is either null (we've destroyed #m_master_channel due to another
   *        error condition) or equals #m_master_channel.
   * @param err_code
   *        The non-success #Error_code from `struc::Channel`.
   */
  void on_master_channel_error(const Master_structured_channel_observer& master_channel_observer,
                               const Error_code& err_code);

  /**
   * In thread W, handler for the successful receipt of log-in response (upon `send()`ing the log-in request),
   * as invoked in CONNECTING state only if the low-level transport::Native_socket_stream::async_connect() succeeded.
   *
   * Post-condition:
   *   - If no init-channels requested by client (async_connect() advanced-use) *nor*
   *     by server (as indicated in `log_in_rsp` itself):
   *     #m_state changed from CONNECTING to either NULL or PEER, depending on whether the log-in
   *     response `log_in_rsp` indicates server side OKed the log-in.  Per the invariant in #m_master_channel doc
   *     header: #m_master_channel is null (state became NULL) or non-null (state became PEER).  In the former case
   *     async_connect() can be attempted again.
   *   - Otherwise (if 1+ such channels, in total, requested):
   *     #m_state unchanged from CONNECTING; on_master_channel_init_open_channel() shall be executed once
   *     per requested (by either side) init-channel generated by opposing server.
   *     - on_master_channel_init_open_channel() (the last one) shall move to PEER state in our stead.
   *
   * @param log_in_rsp
   *        The response in-message from `struc::Channel`.
   * @param init_channels_by_cli_req_pre_sized
   *        See advanced async_connect() features.
   * @param mdt_from_srv_or_null
   *        See advanced async_connect() features.
   * @param init_channels_by_srv_req
   *        See advanced async_connect() features.
   */
  void on_master_channel_log_in_rsp(typename Master_structured_channel::Msg_in_ptr&& log_in_rsp,
                                    Channels* init_channels_by_cli_req_pre_sized, Mdt_reader_ptr* mdt_from_srv_or_null,
                                    Channels* init_channels_by_srv_req);

  /**
   * In thread W, handler for #m_master_channel receiving a init-channel-open message.
   * This is similar to on_master_channel_open_channel_req(), except:
   *   - It occurs in CONNECTING state, after log-in, if server is required to open init-channels
   *     on our behalf or its behalf or both.
   *   - It uses the same in-message, but it is not treated as a request; rather just a notification
   *     of the info regarding 1 of the requested channels...
   *   - ...except for the last one, which *is* treated as a request:
   *     - we set up the regular passive-open-channel handler (on_master_channel_open_channel_req());
   *     - we move to PEER state (emit to #m_conn_on_done_func_or_empty);
   *     - we respond with an OK; so server knows it too can move to PEER state.
   *       - If that `send()` fails we move to NULL instead of PEER state (still invoke on-connect-done handler).
   *
   * So this method is invoked `n_init_channels` times (assuming all goes well); and each time
   * `*init_channels_ptr` grows by 1 element; once it is `n_init_channels` long -- that's the last one,
   * and we do those last 3 sub-bullet points.
   *
   * @param open_channel_msg
   *        The in-message from `struc::Channel`.
   * @param n_init_channels
   *        1+, the count of clients local async_connect() requested plus same from opposing `async_accept()`.
   * @param init_channels_ptr
   *        Intermediate PEER-state #Channel_obj results list; first empty, next time with 1 elements, then 2, ....
   *        If `.size() == n_init_channels`, we shall move to PEER state.
   * @param init_channels_by_cli_req_pre_sized
   *        See advanced async_connect() features.
   * @param mdt_from_srv_or_null
   *        See advanced async_connect() features.
   * @param init_channels_by_srv_req
   *        See advanced async_connect() features.
   * @param mdt_from_srv
   *        Value to which to set `*mdt_from_srv_or_null`, if the latter is not null, and
   *        this is the last invocation of this method.
   */
  void on_master_channel_init_open_channel
         (typename Master_structured_channel::Msg_in_ptr&& open_channel_msg,
          size_t n_init_channels, const boost::shared_ptr<Channels>& init_channels_ptr,
          Channels* init_channels_by_cli_req_pre_sized, Mdt_reader_ptr* mdt_from_srv_or_null,
          Channels* init_channels_by_srv_req, Mdt_reader_ptr&& mdt_from_srv);

  /**
   * In thread W, handler for #m_master_channel receiving a passive-open (a/k/a open-channel-to-client) request.
   * If there is no issue with this request, and we're able to sync-send the open-channel response to that effect,
   * this shall fire on-passive-open handler, giving it a new #Channel_obj in PEER state + metadata.
   * If there is a problem, then it's not a session-hosing situation; local user is none-the-wiser; except that
   * if the `send()` reveals a *new* error, then it is a session-hosing situation, and local user is informed
   * via on-error handler.
   *
   * @param open_channel_req
   *        The request in-message from `struc::Channel`.
   */
  void on_master_channel_open_channel_req(typename Master_structured_channel::Msg_in_ptr&& open_channel_req);

  /**
   * In thread W, based on resources acquired on server side, creates local #Channel_obj to emit to the user
   * thus completing a channel-open.  If active-open on our part, we do this upon receiving server response
   * to our open-channel-to-server request.  If passive-open, we do this upon receiving server's
   * open-channel-to-client request, before replying with our OK.  Either way the in-message from server
   * contained the handle(s) to the resource(s) it acquired for this channel; these are args to this method.
   *
   * @param mq_name_c2s_or_none
   *        If #S_MQS_ENABLED this is the (non-empty) name of the client->server unidirectional MQ for the channel.
   *        Else empty/ignored.
   * @param mq_name_s2c_or_none
   *        Like the preceding but opposite-direction (server->client).
   * @param local_hndl_or_null
   *        If #S_SOCKET_STREAM_ENABLED this is our pre-connected `Native_handle`; the server did `connect_pair()`
   *        and sent us 1/2 of the pair.  Else null/ignored.
   * @param opened_channel_ptr
   *        Target #Channel_obj we shall try to move-to PEER state.  It'll be left unmodified if
   *        error is emitted.
   * @param active_else_passive
   *        `true` if this is from open_channel(); `false` if from on_master_channel_open_channel_req().
   * @param err_code_ptr
   *        Non-null pointer to #Error_code; deref shall be untouched on success (and must be falsy at entry);
   *        else deref shall be set to reason for failure.  As of this writing the only possible path to failure
   *        is if #S_MQS_ENABLED, and we're unable to open an MQ handle or transport::Blob_stream_mq_sender
   *        or transport::Blob_stream_mq_receiver to 1 of the MQs.  This is highly unlikely, since the server
   *        was able to do it fine, but we leave it to caller to deal with the implications.
   *
   * @todo As of this writing the eventuality where Client_session_impl::create_channel_obj() yields an
   * error is treated as assertion-trip-worthy by its caller; hence consider just tripping assertion inside
   * instead and no out-arg.  For now it is left this way in case we'd want the (internal) caller to do something
   * more graceful in the future, and in the meantime it's a decently reusable chunk of code to use in that alleged
   * contingency.
   */
  void create_channel_obj(const Shared_name& mq_name_c2s_or_none, const Shared_name& mq_name_s2c_or_none,
                          util::Native_handle&& local_hndl_or_null, Channel_obj* opened_channel_ptr,
                          bool active_else_passive, Error_code* err_code_ptr);

  /**
   * Little helper that invokes #m_conn_on_done_func_or_empty, passing it `err_code`, and -- per that member's
   * internal use semantics -- empties #m_conn_on_done_func_or_empty.  A key property is that the emptying occurs
   * just *before* invoking it (obviously first saving it).  This allows the handler to itself un-empty
   * #m_conn_on_done_func_or_empty, as required by cancel_peer_state_to_connecting() at least.  (That, in turn,
   * may be used by sub-classes that implement async_connect() by piggybacking onto our vanilla one.)
   *
   * #m_conn_on_done_func_or_empty must not be empty; or behavior is undefined.
   *
   * @param err_code
   *        Arg to `m_conn_on_done_func_or_empty()`.
   */
  void invoke_conn_on_done_func(const Error_code& err_code);

  // Data.

  /**
   * The current state of `*this`.  Accessed in thread W only (not protected by mutex).
   *
   * @note I (ygoldfel) considered just relying on Native_socket_stream
   * itself to just return `false`, and then we'd forward to its state detection.  It would've been nice,
   * but ultimately it just seemed a bit too sloppy and hard to reason about... I ~duplicated this here instead.
   */
  State m_state;

  /**
   * Handles the protocol negotiation at the start of the pipe, as pertains to algorithms perpetuated by
   * the vanilla ipc::session `Session` hierarchy.  Reset essentially at start of each async_connect().
   *
   * Outgoing-direction state is touched when assembling `LogInReq` to send to opposing `Server_session`.
   * Incoming-direction state is touched/verified at the start of interpreting `LogInRsp` receiver from there.
   *
   * @see transport::Protocol_negotiator doc header for key background on the topic.
   */
  transport::Protocol_negotiator m_protocol_negotiator;

  /**
   * Analogous to #m_protocol_negotiator but pertains to algorithms perpetuated by (if relevant)
   * non-vanilla ipc::session `Session` hierarchy implemented on top of our vanilla ipc::session `Session` hierarchy.
   * For example, ipc::session::shm hierarchies can use this to version whatever additional protocol is required
   * to establish SHM things.
   *
   * @see transport::Protocol_negotiator doc header for key background on the topic.
   */
  transport::Protocol_negotiator m_protocol_negotiator_aux;

  /**
   * The `on_done_func` argument to async_connect(); `.empty()` except while in State::S_CONNECTING state.
   * In other words it is assigned at entry to CONNECTING and `.clear()`ed at subsequent entry to State::S_PEER
   * or State::S_NULL (depending on success/failure of the connect op).
   */
  flow::async::Task_asio_err m_conn_on_done_func_or_empty;

  /**
   * Logging-only `*this`-unique ID used in nicknaming the last actively-opened (via open_channel()) channel.
   * It is incremented each time.  Accessed in thread W only (not protected by mutex).
   */
  unsigned int m_last_actively_opened_channel_id;

  /// A-la #m_last_actively_opened_channel_id but for passively-opened channels (on_master_channel_open_channel_req()).
  unsigned int m_last_passively_opened_channel_id;

  /**
   * Worker thread (a/k/a thread W).
   *
   * Ordering: Should be declared before #m_master_channel: It should destruct before the `Task_engine` onto which
   * its queued-up handlers might `post()` items destructs prematurely.
   *
   * ### Why `mutable`? ###
   * Well, session_token() is `const` to the user but must `.post()` which is non-`const`.  This fits the spirit
   * of `mutable`.  I (ygoldfel) think.
   */
  mutable flow::async::Single_thread_task_loop m_async_worker;

  /**
   * The session master channel.  Accessed in thread W only (not protected by mutex).
   *   - When #m_state is NULL, this is null.
   *   - When #m_state is CONNECTING:
   *     - It is still null until transport::Native_socket_stream::sync_connect() yields a PEER-state socket.
   *       (This occurs, if it does occur, within a non-blocking time period upon entry to async_connect().)
   *     - It is not null from that point on -- including through entry to PEER -- but it *does* go back to
   *       null if log-in procedure fails putting `m_state = State::S_NULL` again.
   *   - When #m_state is PEER: It remains non-null and immutable until dtor.
   *
   * ### Rationale for above invariant ###
   * Generally it is a pain in the butt to deal with a `struc::Channel` being destroyed, as it has
   * non-boost.asio-style handler semantics wherein one must be ready for a handler for channel C to fire
   * in our thread W, even after we destroy C in a preceding thread W task.  So, here as in other classes,
   * we aim to keep it non-null and immutable once it's been constructed.  (E.g., that's the case in
   * Server_session_impl.)  However, since we allow async_connect() to be retried on failure, we do need to
   * get back to a coherent state when #m_state is NULL.  So in that case only, it is renullified.
   *
   * Because it is renullified, there's the "observer" thing used in on_master_channel_error() to deal with it.
   * It's a fairly small section of code though.
   */
  Master_structured_channel_ptr m_master_channel;

  /**
   * Null until PEER state is reached, and NULL unless compile-time #S_GRACEFUL_FINISH_REQUIRED is `true`,
   * this is used to block at the start of dtor to synchronize with the opposing `Session` dtor for safety.
   *
   * @see Session_base::Graceful_finisher doc header for all the background ever.
   */
  std::optional<Base::Graceful_finisher> m_graceful_finisher;
}; // class Client_session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_CLI_SESSION_IMPL \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload, \
           schema::ShmType S_SHM_TYPE_OR_NONE>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_CLI_SESSION_IMPL \
  Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload, S_SHM_TYPE_OR_NONE>

TEMPLATE_CLI_SESSION_IMPL
CLASS_CLI_SESSION_IMPL::Client_session_impl(flow::log::Logger* logger_ptr,
                                            const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                            flow::async::Task_asio_err&& on_err_func,
                                            On_passive_open_channel_func&&
                                              on_passive_open_channel_func_or_empty_arg,
                                            std::nullptr_t) :
  Base(cli_app_ref, srv_app_ref, std::move(on_err_func), std::move(on_passive_open_channel_func_or_empty_arg)),

  flow::log::Log_context(logger_ptr, Log_component::S_SESSION),

  m_state(State::S_NULL),

  /* Initial protocol = 1!
   * @todo This will get quite a bit more complex, especially for m_protocol_negotiator_aux,
   *       once at least one relevant protocol gains a version 2.  See class doc header for discussion. */
  m_protocol_negotiator(get_logger(), flow::util::ostream_op_string("cli-sess-", *this), 1, 1),
  m_protocol_negotiator_aux(get_logger(), flow::util::ostream_op_string("cli-sess-aux-", *this), 1, 1),

  m_last_actively_opened_channel_id(0),
  m_last_passively_opened_channel_id(0),
  m_async_worker(get_logger(), flow::util::ostream_op_string("cli_sess[", *this, ']'))
{
  // INFO level would've been okay, but let's just save it for async_connect() which is likely coming soon.
  FLOW_LOG_TRACE("Client session [" << *this << "]: Created.  The session-connect attempt not yet requested.  "
                 "Worker thread starting, will idle until session-connect.");
  m_async_worker.start();
}

TEMPLATE_CLI_SESSION_IMPL
template<typename On_passive_open_channel_handler, typename Task_err>
CLASS_CLI_SESSION_IMPL::Client_session_impl(flow::log::Logger* logger_ptr,
                                            const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                            Task_err&& on_err_func,
                                            On_passive_open_channel_handler&& on_passive_open_channel_func) :
  Client_session_impl(logger_ptr, cli_app_ref, srv_app_ref,
                      flow::async::Task_asio_err(std::move(on_err_func)),
                      On_passive_open_channel_func(std::move(on_passive_open_channel_func)), nullptr)
{
  // Delegated.
}

TEMPLATE_CLI_SESSION_IMPL
template<typename Task_err>
CLASS_CLI_SESSION_IMPL::Client_session_impl(flow::log::Logger* logger_ptr,
                                            const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                            Task_err&& on_err_func) :
  Client_session_impl(logger_ptr, cli_app_ref, srv_app_ref,
                      flow::async::Task_asio_err(std::move(on_err_func)),
                      On_passive_open_channel_func(), nullptr)
{
  // Delegated.
}

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::dtor_async_worker_stop()
{
  using flow::async::Synchronicity;
  using boost::promise;

  // We are in thread U (from either our own dtor or subclass dtor).

  assert((!m_async_worker.task_engine()->stopped()) && "This shall be called at most once.");

  FLOW_LOG_INFO("Client session [" << *this << "]: Shutting down.  Worker thread will be joined, but first "
                "we perform session master channel end-sending op (flush outgoing data) until completion.  "
                "This is generally recommended at EOL of any struc::Channel, on error or otherwise.  "
                "This is technically blocking but unlikely to take long.  Also a Graceful_finisher phase may "
                "precede the above steps (it will log if so).");

  // See Session_base::Graceful_finisher doc header for all the background ever.  Also: it logs as needed.
  if constexpr(S_GRACEFUL_FINISH_REQUIRED)
  {
    Base::Graceful_finisher* graceful_finisher_or_null = {};
    m_async_worker.post([&]() // Have to access m_graceful_finisher (the optional<>) in thread W only.
    {
      // We are in thread W.
      if (m_graceful_finisher)
      {
        graceful_finisher_or_null = &(m_graceful_finisher.value());
      }
    }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); // m_async_worker.post()

    /* If graceful_finisher_or_null is not null, then we're definitely in PEER state, and this is irreversible;
     * so it's cool to ->on_dtor_start() on it.  It'll block as needed, etc.
     *
     * If it *is* null:
     *   - If between the assignment above to null and the next statement, m_graceful_finisher did not become
     *     non-empty, then there's no controversy: It's still not in PEER state; and there's no reason to
     *     block dtor still.  So no need to do anything, and that's correct.  Can just continue.
     *   - If however -- unlikely but possible -- in the last few microseconds (or w/e) indeed m_graceful_finisher
     *     has become non-empty, then now *this is in PEER state.  Yet we still forego any ->on_dtor_start()
     *     and just continue.  Is that wrong?  No: We are in dtor; and the changeover to PEER happened in dtor;
     *     so user has had no chance to grab any resources (like borrowing SHM objects) that would need to
     *     be released before dtor can really-destroy *this (like its SHM arena).  So it's still correct to just
     *     continue. */
    if (graceful_finisher_or_null)
    {
      graceful_finisher_or_null->on_dtor_start();
      // OK, now safe (or as safe as we can guarantee) to blow *this's various parts away.
    }
  } // if constexpr(S_GRACEFUL_FINISH_REQUIRED)

  /* @todo Consider adding blocking struc::Channel::sync_end_sending(), possibly with timeout.  There might be
   * a to-do in its or async_end_sending() class doc header.  Timeout would help us avoid blocking here, unlikely though
   * it is in practice probably.  A sync_ call(), regardless of that, would just be convenient resulting in less
   * code here. */

  /* Let's do the async_end_sending() thing we just promised.
   *
   * So far everything is still operating; so we need to follow normal rules.  m_master_channel access requires
   * being in thread W. */
  m_async_worker.post([&]()
  {
    // We are in thread W.

    if (!m_master_channel)
    {
      return; // Nothing to worry about; async_connect() hasn't even made this yet (or was not invoked).
    }
    // else

    promise<void> done_promise;
    m_master_channel->async_end_sending([&](const Error_code&)
    {
      // We are in thread Wc (unspecified, really struc::Channel async callback thread).
      FLOW_LOG_TRACE("Client session [" << *this << "]: Shutdown: master channel outgoing-direction flush finished.");
      done_promise.set_value();
    });
    // Back here in thread W:
    done_promise.get_future().wait();
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); // m_async_worker.post()
  // Back here in thread U: done; yay.

  /* This (1) stop()s the Task_engine thus possibly preventing any more handlers from running at all (any handler
   * possibly running now is the last one to run); (2) at that point Task_engine::run() exits, hence thread W exits;
   * (3) joins thread W (waits for it to exit); (4) returns.  That's a lot, but it's non-blocking. */
  m_async_worker.stop();
  // Thread W is (synchronously!) no more.  We can now access whatever m_ state we want without concurrency concerns.
} // Client_session_impl::dtor_async_worker_stop()

TEMPLATE_CLI_SESSION_IMPL
CLASS_CLI_SESSION_IMPL::~Client_session_impl()
{
  // We are in thread U.  By contract in doc header, they must not call us from a completion handler (thread W).

  if (!m_async_worker.task_engine()->stopped())
  {
    dtor_async_worker_stop();
  }

  assert(m_conn_on_done_func_or_empty.empty()
         && "async_connect() is used internally from sync_connect() only at this time, so this should be cleared.");
  /* Maintenance note:
   * Please see "CONNECTING state and asynchronicity" in class doc header for key background.
   * As noted there, when/if some form of `*this` becomes network-enabled, async_connect() would probably become
   * a public API that could take blocking amount of time; and if during that time user invokes this dtor, then
   * we would here do our usual thing of creating a one-off thread and triggering m_conn_on_done_func_or_empty
   * with an operation-aborted error code. */
} // Client_session_impl::~Client_session_impl()

TEMPLATE_CLI_SESSION_IMPL
bool CLASS_CLI_SESSION_IMPL::sync_connect(Error_code* err_code)
{
  return sync_connect(mdt_builder(), nullptr, nullptr, nullptr, err_code);
}

TEMPLATE_CLI_SESSION_IMPL
bool CLASS_CLI_SESSION_IMPL::sync_connect(const Mdt_builder_ptr& mdt,
                                          Channels* init_channels_by_cli_req_pre_sized,
                                          Mdt_reader_ptr* mdt_from_srv_or_null,
                                          Channels* init_channels_by_srv_req,
                                          Error_code* err_code)
{
  using flow::async::Task_asio_err;

  Function<bool (Task_asio_err&&)> async_connect_impl_func = [&](Task_asio_err&& on_done_func) -> bool
  {
    return async_connect(mdt, init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req,
                         std::move(on_done_func));
  };
  return sync_connect_impl(err_code, &async_connect_impl_func);

  /* Perf note: The above function-object-wrangling setup does not exactly scream blinding speed... but:
   *   - It is still non-blocking/synchronous.
   *   - Session connect is a rare operation.
   *   - Even disregarding the preceding bullet, the function-object-wrangling is probably minor compared to all
   *     the stuff async_connect() must do.
   * So the code reuse/relative brevity is worth it. */
}

TEMPLATE_CLI_SESSION_IMPL
bool CLASS_CLI_SESSION_IMPL::sync_connect_impl(Error_code* err_code,
                                               Function<bool (flow::async::Task_asio_err&&)>* async_connect_impl_func)
{
  using flow::async::Task_asio_err;
  using boost::promise;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Client_session_impl::sync_connect_impl, _1, async_connect_impl_func);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U.

  // Unless it returns false right away, this completion handler shall execute.
  promise<void> done_promise;
  auto on_done_func = [&](const Error_code& async_err_code)
  {
    // We are in thread W (non-blockingly-soon after async_connect() started).
    *err_code = async_err_code;
    done_promise.set_value();
  };

  /* In something closer to English: this is: async_connect(..., on_done_func), where ... are the args -- except
   * err_code -- passed to calling sync_connect() by user.  Just we're allowing a guy like us, but not necessarily us
   * (for example shm::classic::Client_session_impl), to reuse our little promise-wait technique above. */
  if (!(*async_connect_impl_func)(Task_asio_err(std::move(on_done_func))))
  {
    return false;
  }
  // else

  done_promise.get_future().wait(); // Non-blocking amount of waiting.
  return true;
} // Client_session_impl::sync_connect_impl()

TEMPLATE_CLI_SESSION_IMPL
template<typename Task_err>
bool CLASS_CLI_SESSION_IMPL::async_connect(const Mdt_builder_ptr& mdt,
                                           Channels* init_channels_by_cli_req_pre_sized,
                                           Mdt_reader_ptr* mdt_from_srv_or_null,
                                           Channels* init_channels_by_srv_req,
                                           Task_err&& on_done_func)
{
  using util::process_id_t;
  using flow::util::ostream_op_string;
  using flow::async::Synchronicity;
  using Named_sh_mutex = boost::interprocess::named_mutex;
  using Named_sh_mutex_ptr = boost::movelib::unique_ptr<Named_sh_mutex>;
  using Sh_lock_guard = boost::interprocess::scoped_lock<Named_sh_mutex>;
  using boost::system::system_category;
  using boost::lexical_cast;
  using boost::bad_lexical_cast;
  using boost::movelib::make_unique;
  using boost::make_shared;
  using boost::io::ios_all_saver;
  using fs::ifstream;
  using std::getline;
  using std::string;
  using std::to_string;
  // using ::errno; // It's a macro apparently.

  /* Maintenance note:
   * Please see "CONNECTING state and asynchronicity" in class doc header for key background.
   * Then come back here.  Especially heed this is trying to network-enable this code. */

  // We are in thread U.
  assert((!m_async_worker.in_thread()) && "Do not call directly from its own completion handler.");

  assert(reinterpret_cast<Master_channel_req*>(mdt.get())->m_req_msg.body_root()->isLogInReq()
         && "Did you pass PEER-state-mdt_builder() result to async_connect()?  Use NULL-state one instead.  "
            "Where did you even get it -- another Client_session?  Bad boy.");

  const auto do_func = [&]() -> bool // Little helper that lets us concisely use `return <bool>;`.
  {
    // We are in thread W.

    if (m_state != State::S_NULL)
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Wanted to start "
                       "but already not in NULL state.  Ignoring.");
      return false;
    }
    // else we are going to CONNECTING state.

    m_state = State::S_CONNECTING;
    /* Maintenance note: If/when some form of *this becomes network-enabled (async_connect() becomes public),
     * the following statement will become true: m_conn_on_done_func_or_empty() may now beinvoked from thread W or
     * thread U (dtor) (a race).  Hence dtor code will need to be added to indeed invoke it with operation-aborted
     * code, if it is still not .empty() by then.  That's why -- to make networking-enabling `*this` easier --
     * we even save m_conn_on_done_func_or_empty as opposed to only passing it around in lambda capture(s). */
    m_conn_on_done_func_or_empty = std::move(on_done_func);

    Error_code err_code;

    /* Let's overview our task as the session-opening client.  We are a client, so we open sessions.  In fact
     * *this is expressly for opening 1 (one) session: if they want to open another one, they'd make another
     * Client_session (and moreover it would be an abuse of the concept to do so, to the same Server_app, while
     * *this exists, but I digress, and we don't care about that).  How to open a session?  That's isomorphic to
     * opening a *session master channel* (see session_master_channel.capnp now).  Because it is the channel used
     * for opening user channels, it must be able to transmit native handles (see Native_socket_stream doc header,
     * regaring "way 1" and "way 2" of establishing an N_s_s connection; we're establishing here the thing that
     * will allow the preferred "way 2" to happen subsequently).  So this channel will require at least a
     * Native_socket_stream (handles) pipe.  Would it benefit from MQ pipes?  Probably not; we fully control the
     * schema (no user schema involved yet), and it's fairly small messages only.  So just a single-duplex-handles-pipe
     * channel is fine.  Therefore, all we need to establish before we can load up our Channel<> peer object is
     * a Native_socket_stream peer object in PEER (connected) state.  As mentioned before, there's way 1 and way 2
     * theoretically available; but (as noted) way 2 requires an existing channel for native-handle transmission
     * (chicken/egg), and we lack that.  Hence way 1 is the only one available at the moment.
     *
     * So far, so good; way 1 involves an N_s_s_acceptor listening and doing async_accept()s and an
     * N_s_s cted in NULL state performing a sync_connect().  We're the client, so we'll be doing the latter.
     * Way 1, sadly, means we'll need a name to which to connect.  As discussed elsewhere, all shared resources
     * that are per-session are first segregated by Server_app::m_name (<=> distinct server application); then by
     * *server namespace* within that (<=> distinct server application *instance* a/k/a process <=> PID); and
     * then by session.  Since we're creating the session now, we need the levels of naming above that.
     * We know the Server_app (<=> Server_app::m_name); so we needed the server namespace.  This is stored in
     * the CNS (Current Namespace Store) which is a formal name for the damned PID file (<=> process instance).
     * So read the PID file to get the name.  (Access to this must be synchronized against being written; details
     * below in that step.)
     *
     * Then we'll just sync-connect to the name built off that PID (and the other stuff including Server_app::m_name).
     * As a result we'll have our Native_socket_stream peer object in PEER state.  It is then trivial to wrap it
     * in a one-pipe Channel<>.
     *
     * The session master channel is a structured channel (struc::Channel<> peer object, wrapping the Channel<>,
     * is therefore what we want next).  That's again simple.
     *
     * However, being the session master channel specifically, it has a log-in phase (otherwise optional: and in fact
     * this type of channel is the only one that has one).  So we'll need to perform a quick exchange at the
     * start of the struc::Channel<>'s lifetime.  If that's successful, we are done. */

    // So first step is to read the CNS (Current Namespace Store): the PID file.

    /* PID file can be written-to by the server (that's the whole point), so no choice but to use locking.
     * Various techniques exist, including using the file itself as a lock, but we shall be using a bipc named mutex
     * (can be thought of as a tiny SHM pool storing just the single mutex -- semaphore in Linux).
     * Because it's so small and unique (per app), we won't worry about corruption and cleanup.
     * (@todo Reconsider that.)  Why the separate mutex, instead of using file as a lock?  Answer: in the
     * 1st place, it's arguably easier to use anyway -- the bipc file-lock mechanism is slick but shows the
     * slight oddness of the technique.  Also, for future development: we may need to store other information, such
     * as info on kernel-persistent objects to clean in the even of a crash/abort, in a separate file but
     * accessed in the same critical section as the CNS (PID file).  Hence a common lock would be required for
     * both CNS and these other resources; then it would be odd to use file A as a lock for files A, B, C....
     *
     * One subtlety *is* added due to the decision to use it: ownership and permissions on the mutex.
     * This is discussed in detail in Session_server_impl, where it similarly locks this mutex.
     * The bottom line is we may need to be the ones to create it, hence OPEN_OR_CREATE; and therefore
     * we need to specify permissions; and the permissions value is based on Server_app [sic] despite our being
     * the client.  This is explained in the aforementioned spot in Session_server_impl. */

    string srv_namespace_mv;
    const auto mutex_name = Base::cur_ns_store_mutex_absolute_name();
    const auto mutex_perms = util::shared_resource_permissions(Base::m_srv_app_ref.m_permissions_level_for_client_apps);
    const auto cns_path = Base::cur_ns_store_absolute_path();
    {
      ios_all_saver saver(*(get_logger()->this_thread_ostream())); // Revert std::oct/etc. soon.
      FLOW_LOG_INFO("Client session [" << *this << "]: Session-connect request: Starting session-connector thread "
                    "and connecting.  Presumably the CNS (Current Namespace Store), a/k/a PID "
                    "file [" << cns_path << "] (shared-mutex name [" << mutex_name << "], shared-mutex perms "
                    "[" << std::setfill('0') << std::setw(4) << std::oct << mutex_perms.get_permissions() << "]), "
                    "exists.");
    }

    Named_sh_mutex_ptr sh_mutex;
    util::op_with_possible_bipc_exception(get_logger(), &err_code, error::Code::S_MUTEX_BIPC_MISC_LIBRARY_ERROR,
                                          "Client_session_impl::async_connect()", [&]()
    {
      sh_mutex = make_unique<Named_sh_mutex>(util::OPEN_OR_CREATE, mutex_name.native_str(), mutex_perms);
    });

    if (!err_code)
    {
      Sh_lock_guard sh_lock(*sh_mutex);

      ifstream cns_file(cns_path);
      if (!cns_file.good()) // #1
      {
        const Error_code sys_err_code(errno, system_category());
        FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Could not read CNS (PID) file "
                         "file [" << cns_path << "]; system error details follow.  Immediately giving "
                         "up (non-blocking).");
        FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.

        err_code = sys_err_code;
      }
      else // if (cns_file.good()) // #1
      {
        assert(!err_code);

        /* Read up to a newline.  Be forgiving about whatever is after that; but the newline is required.
         * If it's not there, srv_namespace_mv will be fine (and either way won't include the newline), but .good() will
         * be false. */
        getline(cns_file, srv_namespace_mv);
        if (!cns_file.good())
        {
          FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Opened CNS (PID) file "
                           "file [" << cns_path << "]; but couldn't read a full newline-terminated line.  "
                           "Immediately giving up (non-blocking).");
          err_code = error::Code::S_CLIENT_NAMESPACE_STORE_BAD_FORMAT;
        }
        // else if (cns_file.good()): Fall through. // #2
      } // else if (cns_file.good()) // #1
      // In any case can close file now and unlock.
    } // Sh_lock_guard sh_lock(*sh_mutex); // if (!err_code) (might have become truthy inside though)
    // else if (err_code) { It logged fine already. }

    /* Might as well do this a bit early.  Or no-op if null.  By the way, it does not destroy the mutex itself,
     * just the handle to it.  Destroying it (via Named_sh_mutex::remove()) would be counter to the whole point of it.
     * This does, sort of, leak a bit of RAM but only a bit. */
    sh_mutex.reset();

    if (!err_code)
    {
      // Still OK so far.  Parse all of srv_namespace_mv via lexical_cast<> (which uses `istream >> process_id_t`).
      process_id_t pid = 0; // Initialize to eliminate (false) warning from some compilers.
      try
      {
        pid = lexical_cast<process_id_t>(srv_namespace_mv);
      }
      catch (const bad_lexical_cast& exc)
      {
        FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Opened CNS (PID) file "
                         "file [" << cns_path << "]; read a full newline-terminated line; but could not parse "
                         "it cleanly to an integer PID.  Immediately giving up (non-blocking).  "
                         "Exception detail: [" << exc.what() << "].");
        err_code = error::Code::S_CLIENT_NAMESPACE_STORE_BAD_FORMAT;
      }

      if (!err_code)
      {
        // Might be paranoid but just in case some spaces or smth were in srv_namespace_mv: convert back to eliminate.
        srv_namespace_mv = to_string(pid);
        Base::set_srv_namespace(Shared_name::ct(std::move(srv_namespace_mv)));
        // srv_namespace_mv is now hosed.
        const auto acc_name = Base::session_master_socket_stream_acceptor_absolute_name();

        // Pretty important value; INFO should be okay verbosity-wise.
        FLOW_LOG_INFO("Client session [" << *this << "]: Session-connect request: Opened CNS (PID) file "
                      "file [" << cns_path << "]; obtained server-namespace [" << Base::srv_namespace() << "].  "
                      "Can now initiate Native_socket_stream async-connect based on this name which is "
                      "[" << acc_name << "].");

        /* Create NULL (unconnected) Native_socket_stream peer object and try to get it to PEER (connected).
         * We'll want a sync_io::N_s_s as opposed to N_s_s below; but it does not matter: their APIs are identical
         * until PEER state is reached, and at that point we give it to Channel anyway. */
        transport::sync_io::Native_socket_stream sock_stm(get_logger(),
                                                          ostream_op_string(*this, "->", Base::srv_namespace().str()));
        /* Maintenance note: As noted in "CONNECTING state and asynchronicity" in class doc header:
         * This will need to use a hypothetical sock_stm.async_connect() and become an async step, when/if we
         * network-enable *this in some form.  There *is* no public sock_stm.sync_connect() as of this writing,
         * so if we wanted to write it that form now, we couldn't.  (Whether we would is an orthogonal question but
         * moot.) */
        sock_stm.sync_connect(acc_name, &err_code);

        if (!err_code)
        {
          /* Connect succeeded, but we may quickly encounter problems anyway; so err_code may be truthy after all
           * ultimately.  We have pretty much all we need for the Channel to go into m_master_channel though. */
          FLOW_LOG_INFO
            ("Client session [" << *this << "]: Session-connect request: Master channel Native_socket_stream "
             "sync-connect to [" << acc_name << "] successfully yielded [" << sock_stm << "].  "
             "Wrapping in 1-pipe Channel; then wrapping that in a direct-heap-serializing struc::Channel; "
             "then issuing log-in request.");

          /* Bit of ipc::transport nerdery here:
           *   - struc::Channel needs sync_io-pattern-peer-bearing Channel to upgrade-from;
           *   - specifically we've chosen Socket_stream_channel as that Channel type;
           *   - whose ctor takes a move(sync_io::Native_socket_stream) (sync_io-pattern Native_socket_stream);
           *   - which is why we made a sync_io::Native_socket_stream (not Native_socket_stream) sock_stm above */

          transport::Socket_stream_channel<true>
            unstructured_master_channel(get_logger(), ostream_op_string("smc-", *this), std::move(sock_stm));
          {
            const auto opposing_proc_creds = unstructured_master_channel.remote_peer_process_credentials(&err_code);
            assert((!err_code) && "It really should not fail that early.  If it does look into this code.");
            FLOW_LOG_INFO("Client session [" << *this << "]: Opposing process info: [" << opposing_proc_creds << "].");
          }

          // sock_stm is now hosed.  We have the Channel.

          /* Finally: eat the Channel; make it live inside m_master_channel.  This session master channel requires
           * a log-in phase (the only such channel in the session), so use that ctor. */
          assert(!m_master_channel);
          m_master_channel
            = make_shared<Master_structured_channel>(get_logger(), std::move(unstructured_master_channel),
                                                     transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                                                     false); // We are the client, not the server.
          // unstructured_master_channel is now hosed.

          /* Also enable liveness checking and keep-alive; the other side will do the same.
           * Read comment in Server_session_impl in similar spot; the same applies symmetrically here. */
          m_master_channel->owned_channel_mutable()->auto_ping();
          m_master_channel->owned_channel_mutable()->idle_timer_run();

          m_master_channel->start([this, master_channel_observer = Master_structured_channel_observer(m_master_channel)]
                                    (const Error_code& channel_err_code) mutable
          {
            // We are in thread Wc (unspecified, really struc::Channel async callback thread).
            m_async_worker.post([this,
                                 master_channel_observer = std::move(master_channel_observer),
                                 channel_err_code]()
            {
              on_master_channel_error(master_channel_observer, channel_err_code);
            });
            // @todo Add unexpected response handlers?  One firing would be a bug, so formally shouldn't be needed....
          });

          /* OK!  m_master_channel is rocking... but in the log-in phase (as client).  Follow the quite-rigid steps
           * documented for that phase namely: create (and sync-nb-send) the log-in message and expect log-in
           * response in-message the receipt of which means logged-in phase has been reached.  So this is another
           * async step (we must await the response).  It should be quite quick, as the server is doing its half of
           * the algorithm right now, but I digress.
           *
           * Except, actually, the log-in request message has already been created in mdt_builder(), so as to give
           * user Builder into the metadata sub-field in there.  That should be mutated, as needed, and now
           * we fill out the rest of it and sync-nb-send it. */
          auto& log_in_req_msg = (reinterpret_cast<Master_channel_req*>(mdt.get()))->m_req_msg;

          // Fill out the log-in request.  See SessionMasterChannelMessageBody.LogInReq.
          auto msg_root = log_in_req_msg.body_root()->getLogInReq();

          // Please see "Protocol negotiation" in our class doc header for discussion.
          m_protocol_negotiator.reset(); // In case this is a 2nd, 3rd, ... connect attempt or something.
          m_protocol_negotiator_aux.reset();
          auto proto_neg_root = msg_root.initProtocolNegotiationToServer();
          proto_neg_root.setMaxProtoVer(m_protocol_negotiator.local_max_proto_ver_for_sending());
          proto_neg_root.setMaxProtoVerAux(m_protocol_negotiator_aux.local_max_proto_ver_for_sending());
          /* That was our advertising our capabilities to them.  We'll perform negotiation check at LogInRsp receipt.
           *
           * Maintenance note: This is also remarked upon as of this writing in the schema .capnp file around LogInReq:
           * If we add more versions than 1 in the future, *and* we aim to be backwards-compatible with at least 1
           * older protocol version, then we must be careful in the below LogInReq setter code; as at *this* time we
           * don't yet know server side's capabilities (not until LogInRsp!), hence we can't know which version to
           * speak.  So until then (which isn't long) we need to be protocol-version-agnostic. */

          msg_root.setMqTypeOrNone(S_MQ_TYPE_OR_NONE);
          msg_root.setNativeHandleTransmissionEnabled(S_TRANSMIT_NATIVE_HANDLES);
          msg_root.setShmTypeOrNone(S_SHM_TYPE);
          // .metadata is already filled out.
          msg_root.setNumInitChannelsByCliReq(init_channels_by_cli_req_pre_sized
                                                ? init_channels_by_cli_req_pre_sized->size()
                                                : 0);

          auto claimed_own_proc_creds = msg_root.initClaimedOwnProcessCredentials();
          claimed_own_proc_creds.setProcessId(util::Process_credentials::own_process_id());
          claimed_own_proc_creds.setUserId(util::Process_credentials::own_user_id());
          claimed_own_proc_creds.setGroupId(util::Process_credentials::own_group_id());
          // Our ClientApp::m_name (srv has list of allowed ones):
          msg_root.initOwnApp().setName(Base::cli_app_ptr()->m_name);

          // Synchronously, non-blockingly, can't-would-blockingly send it.  This can fail (it will log WARNING if so).
          auto on_log_in_rsp_func
            = [this, init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req]
                (typename Master_structured_channel::Msg_in_ptr&& log_in_rsp) mutable
          {
            // We are in thread Wc (as in above error handler).
            m_async_worker.post([this, log_in_rsp = std::move(log_in_rsp),
                                 init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req]
                                  () mutable
            {
              on_master_channel_log_in_rsp(std::move(log_in_rsp),
                                           init_channels_by_cli_req_pre_sized,
                                           mdt_from_srv_or_null,
                                           init_channels_by_srv_req);
            });
          };
          /* Maintenance note: (Background: "CONNECTING state and asynchronicity" in class doc header.)
           * We could use sync_request() here, leading to simpler code.  As mentioned that doc-section, though,
           * leaving it in ->async_request() form for when/if *this (in some form) becomes network-enabled.
           * We already have the async error handling all set-up and wouldn't need to change write much more, etc. */
          if (!m_master_channel->async_request(log_in_req_msg, nullptr, nullptr, // One-off (single response).
                                               std::move(on_log_in_rsp_func), &err_code))
          {
            /* The docs say async_request() returns false if:
             * "`msg` with-handle, but #Owner_channel has no handles pipe; an #Error_code was previously emitted
             * via on-error handler; async_end_sending() has already been called."  Only the middle one is possible in
             * our case.  on_master_channel_error() shall issue m_conn_on_done_func_or_empty(truthy) in our stead. */
            FLOW_LOG_TRACE("send() synchronously failed; must be an error situation.  Error handler should "
                           "catch it shortly or has already caught it.");
            return true; // <-- Attention!  Async-op started (error handler coming).
          }
          // else

          if (!err_code)
          {
            FLOW_LOG_TRACE("Client session [" << *this << "]: Log-in request issued.  Awaiting log-in response.  "
                           "CONNECTING state still in effect.");
            return true; // <-- Attention!  Async-op started (completion handler coming).
          }
          // else

          /* Back to the drawing board (no async-op started); and while at the drawing board the master channel is
           * null; so make it so. err_code is set. */
          m_master_channel.reset();
          // Fall through.
        } // if (!err_code) [from sock_stm.sync_connect()] (might have become truthy inside though)
        else // if (err_code) [from m_master_channel->async_request()]
        {
          FLOW_LOG_WARNING
            ("Client session [" << *this << "]: Session-connect request: Master channel Native_socket_stream "
             "sync-connect to [" << acc_name << "] failed (code "
             "[" << err_code << "] [" << err_code.message() << "]).  Will go back to NULL state and report "
             "to user via on-async-connect handler.");
          // Fall through.
        }
      } // if (!err_code) [from: srv_namespace_mv -> `pid` conversion] (might have become truthy inside though)
      // else if (err_code) { Fall through. }
    } // if (!err_code) [from: reading srv_namespace_mv from CNS file] (might have become truthy inside though)
    // else if (err_code) { Fall through. }

    assert(err_code && "All code paths except async-op-starting ones (above) must have led to a truthy Error_code.");

    // Can emit.
    assert((m_state == State::S_CONNECTING)
           && "A lot of (albeit synchronous) code above... just making sure.");
    assert((!m_conn_on_done_func_or_empty.empty())
           && "m_conn_on_done_func_or_empty can only be emptied upon executing it.");

    m_state = State::S_NULL;

    FLOW_LOG_TRACE("Client session [" << *this << "]: Session-connect request: Reporting connect failed "
                   "(it was right inside async_connect(), before actual socket-stream connect).  Executing handler.");
    invoke_conn_on_done_func(err_code);

    return true;
  }; // const auto do_func =

  // We post all real work onto our thread W.

  bool result;
  m_async_worker.post([&]() { result = do_func(); },
                      Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

  return result;
} // Client_session_impl::async_connect()

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::on_master_channel_log_in_rsp
       (typename Master_structured_channel::Msg_in_ptr&& log_in_rsp, Channels* init_channels_by_cli_req_pre_sized,
        Mdt_reader_ptr* mdt_from_srv_or_null, Channels* init_channels_by_srv_req)
{
  using boost::shared_ptr;
  using boost::make_shared;
  using std::string;

  // We are in thread W.

  /* Cool; we got the response to the log-in request we sent.  m_master_channel is actually now in logged-in phase
   * already; but we aren't home free quite yet.  Firstly there's a datum in there we need; but also -- as documented
   * in struc::Channel doc headers -- the log-in response is considered valid by it (in terms of moving to
   * logged-in phase) as long as it, in fact, responds to the request; but there's no check for a *particular*
   * response message, just as long as it's a response.  So that's on us, and if that's not right, then we can
   * still fail our CONNECTING->PEER state transition and go to NULL instead after all. */

  Error_code err_code; // Success to start but...
  // (Initialize to avoid -- incorrect -- warning.)
  size_t n_init_channels = 0; // If successful log-in (so far) this'll be set to how many init channels we'll expect.
  Mdt_reader_ptr mdt_from_srv; // If successful log-in (so far) this'll be set to eventual *mdt_from_srv_or_null result.
  const auto& body_root = log_in_rsp->body_root();
  if (!body_root.isLogInRsp())
  {
    FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Log-in response received, but it is "
                     "not the expected LogInRsp message -- other side misbehaved?  Will go back to NULL "
                     "state and report to user via on-async-connect handler.");
    err_code = error::Code::S_CLIENT_MASTER_LOG_IN_RESPONSE_BAD;
  }
  else // if (isLogInRsp())
  {
    const auto log_in_rsp_root = body_root.getLogInRsp();

    /* Please see similar code on server side; they will have made the same check before issuing the LogInRsp to us.
     * Protocol-negotiation occurs *here*, before any further fields are interpreted. */
    const auto proto_neg_root = log_in_rsp_root.getProtocolNegotiationToClient();
    m_protocol_negotiator.compute_negotiated_proto_ver(proto_neg_root.getMaxProtoVer(), &err_code);
    if (!err_code)
    {
      m_protocol_negotiator_aux.compute_negotiated_proto_ver(proto_neg_root.getMaxProtoVerAux(), &err_code);
      if (!err_code)
      {
        /* As of this writing there is only protocol version 1.  In the future, if we add more versions *and*
         * decide to be backwards-compatible with older versions, then this is the point from which one knows
         * which protocol-version to speak (query m_protocol_negotiator.negotiated_proto_ver(); other Session
         * hierarchies can query m_protocol_negotiator_aux.<same>()). */

        const auto n_init_channels_by_srv_req = log_in_rsp_root.getNumInitChannelsBySrvReq();
        if ((n_init_channels_by_srv_req != 0) && (!init_channels_by_srv_req))
        {
          FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Log-in response received, "
                           "and it is the expected LogInRsp message; but this side passed null container for "
                           "init-channels-by-srv-request, while server indicates it will open "
                           "[" << n_init_channels_by_srv_req << "] of them -- this or other side misbehaved?  "
                           "Will go back to NULL state and report to user via on-async-connect handler.");
          err_code = error::Code::S_INVALID_ARGUMENT;
        }
        else // if (n_init_channels_by_srv_req is fine)
        {
          auto cli_namespace_mv = Shared_name::ct(string(log_in_rsp_root.getClientNamespace()));
          if (cli_namespace_mv.empty() || (cli_namespace_mv == Shared_name::S_SENTINEL))
          {
            FLOW_LOG_WARNING("Client session [" << *this << "]: Session-connect request: Log-in response received, "
                             "and it is the expected LogInRsp message; but the cli-namespace "
                             "[" << cli_namespace_mv << "] therein is illegal -- other side misbehaved?  Will go "
                             "back to NULL state and report to user via on-async-connect handler.");
            err_code = error::Code::S_CLIENT_MASTER_LOG_IN_RESPONSE_BAD;
          }
          else // if (all good)
          {
            Base::set_cli_namespace(std::move(cli_namespace_mv));
            // cli_namespace_mv is now hosed.

            // All is cool; can compute this now.
            n_init_channels = n_init_channels_by_srv_req + (init_channels_by_cli_req_pre_sized
                                                              ? init_channels_by_cli_req_pre_sized->size()
                                                              : 0);

            FLOW_LOG_INFO("Client session [" << *this << "]: Session-connect request: Log-in response received, and "
                          "it is the expected LogInRsp message; cli-namespace [" << Base::cli_namespace() << "] "
                          "received and saved.  Init-channel count expected is [" << n_init_channels << "]; of these "
                          "[" << n_init_channels_by_srv_req << "] requested by opposing server.  "
                          "If 0: Will go to PEER state and report to user via on-async-connect handler.  "
                          "Else: Will await that many open-channel-to-client requests with the init-channel info.");

            if (mdt_from_srv_or_null)
            {
              /* They're interested in the metadata from server; we can now fish it out (but only set
               * *mdt_from_srv_or_null when invoking handler on success as promised; untouched otherwise as promised).
               * So prepare the result.
               *
               * Use the same technique as when emitting `mdt` in on_master_channel_open_channel_req(); omitting
               * comments. */
              struct Rsp_and_mdt_reader
              {
                typename Mdt_reader_ptr::element_type m_mdt_reader;
                typename Master_structured_channel::Msg_in_ptr m_rsp;
              };
              shared_ptr<Rsp_and_mdt_reader> rsp_and_mdt(new Rsp_and_mdt_reader
                                                               { log_in_rsp_root.getMetadata(),
                                                                  std::move(log_in_rsp) });
              // (log_in_rsp is now hosed.)
              mdt_from_srv = Mdt_reader_ptr(std::move(rsp_and_mdt), &rsp_and_mdt->m_mdt_reader);
            } // if (mdt_from_srv_or_null)
            // else if (!mdt_from_srv_or_null) { Leave mdt_from_srv null too. }
          } // if (all good, continue on path to PEER state)
        } // else if (n_init_channels_by_srv_req is fine) (but err_code may have become truthy inside)
      } // else if (!err_code from m_protocol_negotiator_aux.compute_negotiated_proto_ver()) (ditto re. err_code)
    } // else if (!err_code from m_protocol_negotiator.compute_negotiated_proto_ver()) (ditto re. err_code)
  } // if (isLogInRsp())

  assert((m_state == State::S_CONNECTING)
         && "Once master-channel is up, there are 2 ways to exit CONNECTING state: 1, "
              "on_master_channel_log_in_rsp(): we are it; 2, on_master_channel_error(), but if that has been "
              "called already, then we (on_master_channel_log_in_rsp()) should not have been.");
  assert((!m_conn_on_done_func_or_empty.empty())
         && "m_conn_on_done_func_or_empty can only be emptied upon executing it.");

  if (err_code)
  {
    /* Wrong log-in response, so as far as they're concerned, it's no different from the async-connect failing.  Back
     * to the drawing board; and while at the drawing board the master channel is null.  err_code is set. */
    m_master_channel.reset();
    m_state = State::S_NULL;
    FLOW_LOG_TRACE("Client session [" << *this << "]: Session-connect request: Master channel Native_socket_stream "
                   "async-connect succeded, but the log-in within the resulting channel failed "
                   "(see above for details).  Executing handler.");
    invoke_conn_on_done_func(err_code);

    // That's it.  They can try async_connect() again.
    return;
  } // if (err_code)

  if (n_init_channels == 0)
  {
    // Cool -- nothing else to be done (no init-channels to open).  Emit stuff.

    if (init_channels_by_srv_req)
    {
      init_channels_by_srv_req->clear();
    }
    assert(((!init_channels_by_cli_req_pre_sized) || init_channels_by_cli_req_pre_sized->empty())
           && "Otherwise could n_init_channels remain 0?");
    assert((bool(mdt_from_srv_or_null) == bool(mdt_from_srv)) && "Some logic above must be broken.");
    if (mdt_from_srv_or_null)
    {
      *mdt_from_srv_or_null = std::move(mdt_from_srv);
    }

    /* From now on we can handle passive-open-channel requests.  (They might even be cached inside
     * m_master_channel now already, and they'll be flushed right now.) */
    bool ok = m_master_channel->expect_msgs(Master_structured_channel::Msg_which_in::OPEN_CHANNEL_TO_CLIENT_REQ,
                                            [this](typename Master_structured_channel::Msg_in_ptr&& open_channel_req)
    {
      // We are in thread Wc (unspecified, really struc::Channel async callback thread).
      m_async_worker.post([this, open_channel_req = std::move(open_channel_req)]() mutable
      {
        // We are in thread W.
        on_master_channel_open_channel_req(std::move(open_channel_req));
      });
    });

    if (!ok)
    {
      // Got hosed very recently (that logged).  on_master_channel_error() will deal with it.
      return;
    }

    m_state = State::S_PEER;

    FLOW_LOG_TRACE("Client session [" << *this << "]: Session-connect request: Master channel Native_socket_stream "
                   "async-connect succeded, and the log-in within the resulting channel succeeded.  "
                   "Executing handler.");
    invoke_conn_on_done_func(err_code);

    // That's it.  They can proceed with session work.  (Unless subclass just did a cancel_peer_state_to_*().)

    /* See Session_base::Graceful_finisher doc header for all the background ever.
     *
     * Why didn't we do the following just above, after setting state to PEER?  It is indeed unusual to do something
     * in thread W *after* invoking on-done handler.  The immediate reason is the possibility of subclass,
     * not user, invoking cancel_peer_state_to_*() -- so we're not in PEER state "after all": but we will
     * certainly then do it upon complete_async_connect_after_canceling_peer_state(<success>) reaching PEER state.
     *
     * Update: I (ygoldfel, who wrote this) am very uncomfortable with this.  This is a consequence of the, in
     * retrospect, house-of-cards-like way vanilla Client_session_impl is reused by the SHM-based guys; and that's
     * easily the biggest impl detail I regret to date -- the rest I'm quite happy with really.
     * So I'm addeding an assert here -- and I know as of this writing it won't trip, simply because
     * S_GRACEFUL_FINISH_REQUIRED is true only for SHM-jemalloc, and SHM-jemalloc will 100%
     * do cancel_peer_state_to_connecting() inside on_done_func() (as of this writing).  So rather than execute
     * code I am skeptical about, safety-wise (kind of thing that'll work 99.9999% of the time at least, but
     * is just too weird), I'll blow up if somehow later changes dislodge this balance.
     *
     * @todo OMG, rejigger how vanilla Client_session_impl is reused by the SHM-based subclasses: This "undo PEER
     * state paradigm" is just ridiculous.  We can do better than this. */
    if constexpr(S_GRACEFUL_FINISH_REQUIRED)
    {
      assert((m_state != State::S_PEER) &&
             "In practice only SHM-jemalloc should use GRACEFUL_FINISH_REQUIRED=true, and that guy should have "
               "canceled PEER state via cancel_peer_state_to_connecting() at best; see above comment and to-do.");
      // Commented out: m_graceful_finisher.emplace(get_logger(), this, &m_async_worker, m_master_channel.get());
    }

  } // // if (n_init_channels == 0)
  else // if (n_init_channels != 0)
  {
    /* Now we await openChannelToClientReq (x N) in similar fashion to PEER state,
     * except it won't be request(s) really but just the resource info/handles that normally come in such requests;
     * we need not respond.
     *
     * We have to postpone the move to PEER and firing of on_done_func() until that's done, so
     * expect open-channel requests similarly to the above; but a slightly different handler that knows to
     * finish up the async_connect() op.  Keeping comments light as it's similar to above.
     *
     * Since we promised not to touch the async out-args until success, set up a ref-counted target container
     * used as an intermediate. */
    auto init_channels = make_shared<Channels>();
    init_channels->reserve(n_init_channels);

    m_master_channel->expect_msgs
      (Master_structured_channel::Msg_which_in::OPEN_CHANNEL_TO_CLIENT_REQ,
       [this, n_init_channels, init_channels = std::move(init_channels),
        init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null, init_channels_by_srv_req,
        mdt_from_srv = std::move(mdt_from_srv)]
         (typename Master_structured_channel::Msg_in_ptr&& open_channel_msg) mutable
    {
      m_async_worker.post
        ([this, open_channel_msg = std::move(open_channel_msg),
          // Copy init_channels, mdt_from_srv, as this may run 2+ times.
          n_init_channels, init_channels, init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null,
          init_channels_by_srv_req, mdt_from_srv]
           () mutable
      {
        on_master_channel_init_open_channel(std::move(open_channel_msg),
                                            n_init_channels, init_channels,
                                            init_channels_by_cli_req_pre_sized, mdt_from_srv_or_null,
                                            init_channels_by_srv_req, std::move(mdt_from_srv));
      });
    });
  } // else if (n_init_channels != 0)
} // Client_session_impl::on_master_channel_log_in_rsp()

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::on_master_channel_init_open_channel
       (typename Master_structured_channel::Msg_in_ptr&& open_channel_msg,
        size_t n_init_channels, const boost::shared_ptr<Channels>& init_channels_ptr,
        Channels* init_channels_by_cli_req_pre_sized, Mdt_reader_ptr* mdt_from_srv_or_null,
        Channels* init_channels_by_srv_req, Mdt_reader_ptr&& mdt_from_srv)
{
  using schema::detail::OpenChannelResult;
  using flow::util::ostream_op_string;
  using std::string;

  // We are in thread W.

  assert((m_state == State::S_CONNECTING)
         && "Once master-channel is up/past log-in there are 2 ways to exit CONNECTING state: 1, "
              "on_master_channel_init_open_channel(): we are it; 2, on_master_channel_error(), but if that has been "
              "called already, then we (on_master_channel_init_open_channel()) should not have been.");
  assert((!m_conn_on_done_func_or_empty.empty())
         && "m_conn_on_done_func_or_empty can only be emptied upon executing it.");

  const auto root = open_channel_msg->body_root().getOpenChannelToClientReq();
  auto& init_channels = *init_channels_ptr;

  // Cool! We can open our side of each channel component based on the stuff server prepared and sent to us.

  auto local_hndl_or_null = open_channel_msg->native_handle_or_null();
  const auto mq_name_c2s_or_none = Shared_name::ct(string(root.getClientToServerMqAbsNameOrEmpty()));
  const auto mq_name_s2c_or_none = Shared_name::ct(string(root.getServerToClientMqAbsNameOrEmpty()));

  Error_code err_code;
  Channel_obj opened_channel;
  create_channel_obj(mq_name_c2s_or_none, mq_name_s2c_or_none, std::move(local_hndl_or_null),
                     &opened_channel, false, // false => passive.
                     &err_code);
  // err_code is either success or failure, and opened_channel is either blank or a PEER-state Channel.

  assert((!err_code)
         && "As of this writing the only path to this is inside the MQ-related guts of Channel creation (if "
              "MQs are even configured), namely because something in Blob_stream_mq_base_impl indicates "
              "there'd be no more than 1 MQ receiver or 1 MQ sender for a given MQ (c2s or s2c) "
              "and that is *after* server was able to get through its side of the same thing not to mention "
              "create the MQs in the first place.  So it is very strange no-man's-land; we abort.  @todo Reconsider.");

  // Great!  opened_channel is ready (in PEER state).  Add it to intermediate container init_channels.
  assert(init_channels.size() < n_init_channels);
  init_channels.emplace_back(std::move(opened_channel));

  if (init_channels.size() != n_init_channels)
  {
    FLOW_LOG_TRACE("Client session [" << *this << "]: Passive init-open-channel: Successful on this side!  "
                   "Of expected total [" << n_init_channels << "] channels we have set-up "
                   "[" << init_channels.size() << "]; more shall be coming soon.  "
                   "We save local peer Channel object [" << opened_channel << "] for now.");
    return;
  }
  // else if (init_channels has n_init_channels elements)

  /* Don't forget to set-up the normal open-channel-to-client-request handler now (same as the no-init-channels
   * path in on_master_channel_log_in_rsp() -- which we did not take).  Keeping comments light. */
  if (!m_master_channel->undo_expect_msgs(Master_structured_channel::Msg_which_in::OPEN_CHANNEL_TO_CLIENT_REQ))
  {
    // Got hosed very recently (that logged).  on_master_channel_error() will deal with it.
    return;
  }
  // else

  bool ok = m_master_channel->expect_msgs(Master_structured_channel::Msg_which_in::OPEN_CHANNEL_TO_CLIENT_REQ,
                                         [this](typename Master_structured_channel::Msg_in_ptr&& open_channel_req)
  {
    m_async_worker.post([this, open_channel_req = std::move(open_channel_req)]() mutable
    {
      on_master_channel_open_channel_req(std::move(open_channel_req));
    });
  });
  if (!ok)
  {
    return; // Again (see undo_expect_msgs() path above).
  }
  // else

  // As planned, respond to the last one.
  auto open_channel_rsp = m_master_channel->create_msg();
  open_channel_rsp.body_root()->initOpenChannelToClientRsp().setOpenChannelResult(OpenChannelResult::ACCEPTED);
  if (!m_master_channel->send(open_channel_rsp, open_channel_msg.get(), &err_code))
  {
    return; // Again (see undo_expect_msgs() path above).
  }
  // else
  if (err_code)
  {
    // Sigh.  send() emitted error: we must handle it.  Go to NULL state as, e.g., if log-in request send() fails.

    m_state = State::S_NULL;
    FLOW_LOG_TRACE("Client session [" << *this << "]: Session-connect request: Master channel Native_socket_stream "
                   "async-connect succeeded, and the log-in request sending "
                   "succeeded, and the log-in response was received OK, and then channel-opening "
                   "succeeded, but the send of the OK response to the last "
                   "channel info notification failed (see above for details).  Executing handler.");
    invoke_conn_on_done_func(err_code);
    return;
  } // if (err_code)

  // Whew!  Nothing can go wrong now; we shall go to PEER state.  Emit results via out-args.

  FLOW_LOG_INFO("Client session [" << *this << "]: Passive init-open-channel: Successful on this side!  "
                "We now have the expected total [" << n_init_channels << "] channels.  Shall emit them "
                "(see next log messages) and metadata.  Will go to PEER state and report to user via "
                "on-async-connect handler.");
  size_t idx = 0;
  if (init_channels_by_cli_req_pre_sized)
  {
    for (auto& init_channel_ref : *init_channels_by_cli_req_pre_sized)
    {
      init_channel_ref = std::move(init_channels[idx]);
      FLOW_LOG_INFO("Client session [" << *this << "]: Emitting init-channel (cli-requested): "
                    "[" << init_channel_ref << "].");
      ++idx;
    }
  }
  if (init_channels_by_srv_req)
  {
    auto& init_channels_by_srv_req_ref = *init_channels_by_srv_req;
    init_channels_by_srv_req_ref.clear();
    init_channels_by_srv_req_ref.reserve(n_init_channels - idx);
    for (; idx != n_init_channels; ++idx)
    {
      init_channels_by_srv_req_ref.emplace_back(std::move(init_channels[idx]));
      FLOW_LOG_INFO("Client session [" << *this << "]: Emitting init-channel (srv-requested): "
                    "[" << init_channels_by_srv_req_ref.back() << "].");
    }
  }
  assert(idx == n_init_channels);
  // OK: Channel out-args all the way from async_connect() have been set.  Now same for mdt; and invoke handler.

  assert((bool(mdt_from_srv_or_null) == bool(mdt_from_srv)) && "Some logic in log-in handler must be broken.");
  if (mdt_from_srv_or_null)
  {
    *mdt_from_srv_or_null = std::move(mdt_from_srv);
  }

  // Halle-freakin'-lujah!

  m_state = State::S_PEER;

  FLOW_LOG_TRACE("Client session [" << *this << "]: Session-connect request: Master channel Native_socket_stream "
                 "async-connect succeded, and the log-in within the resulting channel succeeded, and the init-channel "
                 "opening succeeded.  Executing handler.");
  invoke_conn_on_done_func(err_code);

  // That's it.  They can proceed with session work.  (Unless subclass just did a cancel_peer_state_to_*().)

  // Same stuff as in the no-init-channels path, where state becomes PEER.  *Please* see key comments there.
  if constexpr(S_GRACEFUL_FINISH_REQUIRED)
  {
    assert((m_state != State::S_PEER) &&
           "In practice only SHM-jemalloc should use GRACEFUL_FINISH_REQUIRED=true, and that guy should have "
             "canceled PEER state via cancel_peer_state_to_connecting() at best; see above comment and to-do.");
    // Commented out: m_graceful_finisher.emplace(get_logger(), this, &m_async_worker, m_master_channel.get());
  }
} // Client_session_impl::on_master_channel_init_open_channel()

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::invoke_conn_on_done_func(const Error_code& err_code)
{
  /* As promised we'll .clear() the member *before* invoking it; so that the handler itself -- as of this writing
   * only in the case of a sub-class (e.g., an ipc::session::shm:: thing) async_connect() calling our own and
   * substituting its own wrapper around the user's actual handler -- can un-.clear() it for its own nefarious
   * purposes. */

  auto func = std::move(m_conn_on_done_func_or_empty);
  m_conn_on_done_func_or_empty.clear();

  func(err_code);
  FLOW_LOG_TRACE("Handler finished.");
}

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::on_master_channel_error
       (const Master_structured_channel_observer& master_channel_observer,
        const Error_code& err_code)
{
  // We are in thread W.

  auto master_channel = master_channel_observer.lock();
  if (!master_channel)
  {
    FLOW_LOG_TRACE("Client session [" << *this << "]: Session master channel reported error; but the source "
                   "master channel has since been destroyed; so presumably the log-in response was received "
                   "from remote server, and it was invalid, so the channel was destroyed by our log-in response "
                   "handler; but coincidentally before that destruction could occur an incoming-direction "
                   "channel error occurred.  The error does not matter anymore.  Ignoring.");
    return;
  }
  // else

  assert((master_channel == m_master_channel)
         && "As of this writing m_master_channel is the only referrer to the underlying master channel; "
              "hence if the observer still refers to *something* it must refer to m_master_channel.");
  master_channel.reset();
  // From this point on just rely on m_master_channel.  (It is only touched in thread W and needs no lock.)

  assert((m_state != State::S_NULL) && "State cannot change to NULL while keeping m_master_channel alive.");

  if (m_state == State::S_CONNECTING)
  {
    FLOW_LOG_WARNING("Client session [" << *this << "]: Log-in: Session master channel reported error "
                     "[" << err_code << "] [" << err_code.message() << "].  Will go back to NULL "
                     "state and report to user via on-async-connect handler.");

    /* Channel incoming-direction error while awaiting log-in response or init-channel-open(s) or even earlier,
     * so as far as they're concerned, it's no different from the async-connect failing.  Back
     * to the drawing board; and while at the drawing board the master channel is null. */
    m_master_channel.reset();

    assert((!m_conn_on_done_func_or_empty.empty())
           && "m_conn_on_done_func_or_empty can only be emptied upon executing it.");

    m_state = State::S_NULL;

    FLOW_LOG_TRACE("Client session [" << *this << "]: Executing on-connect handler.");
    invoke_conn_on_done_func(err_code);
    return;
  } // if (m_state == State::S_CONNECTING)
  // else

  assert(m_state == State::S_PEER);
  /* Session master channel reported channel-hosing incoming-direction error.  Emit it to user if and only if
   * we haven't already emitted some error.  If we have, it shouldn't have come from either
   * m_master_channel->send()/etc. (synchronously) or a previous on_master_channel_error(): struc::Channel promises
   * to emit any channel-hosing error at most once, through exactly one of those mechanisms.  It is still
   * possible, though: There are sources of error/Error_codes emitted in *this Client_session that don't come
   * from m_master_channel but our own logic.  (As of this writing, I believe, that was only actually before PEER
   * state; but that could easily change as we maintain the code.) */
  if (!Base::hosed())
  {
    Base::hose(err_code);
  }

  if constexpr(S_GRACEFUL_FINISH_REQUIRED)
  {
    m_graceful_finisher->on_master_channel_hosed();
  }
} // Client_session_impl::on_master_channel_error()

TEMPLATE_CLI_SESSION_IMPL
const Session_token& CLASS_CLI_SESSION_IMPL::session_token() const
{
  using flow::async::Synchronicity;

  // We are in thread U.

  const Session_token* result;

  m_async_worker.post([&]()
  {
    // We are in thread W.

    if (m_state != State::S_PEER)
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Session-token accessor: Invoked before PEER state. "
                       "Returning null.");
      result = &(transport::struc::NULL_SESSION_TOKEN);
      return;
    }
    // else if (m_state == State::S_PEER):

    if (Base::hosed())
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Session-token accessor: Core executing after session "
                       "hosed.  Ignoring.");
      result = &(transport::struc::NULL_SESSION_TOKEN);
      return;
    }
    // else

    assert(m_master_channel);
    result = &(m_master_channel->session_token());
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

  return *result;
} // Client_session_impl::session_token()

TEMPLATE_CLI_SESSION_IMPL
typename CLASS_CLI_SESSION_IMPL::Mdt_builder_ptr CLASS_CLI_SESSION_IMPL::mdt_builder() const
{
  using flow::async::Synchronicity;

  // We are in thread U.

  typename Master_structured_channel::Msg_out req_msg;
  bool connect_else_open;
  bool ok = false;
  m_async_worker.post([&]()
  {
    // We are in thread W: have to be for both m_state and m_master_channel access.

    if (m_state == State::S_CONNECTING)
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open metadata request: Invoked while CONNECTING; "
                       "must be in NULL state before async_connect() or in PEER state before open_channel().  "
                       "Ignoring.");
      return;
    }
    // else
    connect_else_open = m_state == State::S_NULL;

    /* OK, so we really want something like m_master_channel->create_msg() (then we'll fill it out and return
     * alias-pointer into its metadata sub-builder).  Before CONNECTING, though, there is no m_master_channel
     * yet.  That's really a convenience, though; create_msg() (per its docs) merely supplies the little
     * Builder_config stored in m_master_channel.  We can make one without the channel easily enough.
     * Granted it's slightly annoying (and some logic from struc::Channel is repeated; @todo improve) but w/e. */
    const typename Master_structured_channel::Builder_config
      builder_config{ get_logger(), transport::sync_io::Native_socket_stream::S_MAX_META_BLOB_LENGTH, 0, 0 };

    req_msg = decltype(req_msg)(builder_config); // Construct it.  We've done same thing as ->create_msg().
    ok = true;
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

  // The rest we can do back in thread U again.

  if (!ok)
  {
    // As advertised: Previous error (reported via on-error handler) => return null.
    return Mdt_builder_ptr();
  }
  // else

  const auto mdt_root
    = connect_else_open ? req_msg.body_root()->initLogInReq().initMetadata()
                        : req_msg.body_root()->initOpenChannelToServerReq().initMetadata();

  Master_channel_req_ptr req_ptr(new Master_channel_req{ mdt_root, std::move(req_msg) });
  return Mdt_builder_ptr(std::move(req_ptr), &req_ptr->m_mdt_builder);
} // Client_session_impl::mdt_builder()

TEMPLATE_CLI_SESSION_IMPL
bool CLASS_CLI_SESSION_IMPL::open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt,
                                          Error_code* err_code)
{
  using util::Native_handle;
  using schema::detail::OpenChannelResult;
  using flow::async::Synchronicity;
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using std::string;

  // We are in thread U.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Client_session_impl::open_channel,
                                     target_channel, flow::util::bind_ns::cref(mdt), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (!mdt)
  {
    FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open active request: "
                     "null metadata-builder passed-in; either user error passing in null; or more likely "
                     "that routine failed and returned null, meaning an incoming-direction error was or is being "
                     "emitted to the on-error handler.  Emitting code-less failure in this synchronous path.");
    return false;
  }
  // else

  FLOW_LOG_INFO("Client session [" << *this << "]: Channel open active request: The blocking, but presumed "
                "sufficiently quick to be considered non-blocking, request-response exchange initiating; timeout set "
                "to a generous [" << round<milliseconds>(Base::S_OPEN_CHANNEL_TIMEOUT) << "].");

  /* As usual do all the work in thread W, where we can access m_master_channel and m_state among other things.
   * We do need to return the results of the operation here in thread U though. */
  bool sync_error_but_do_not_emit = false; // If this ends up true, err_code is ignored and clear()ed ultimately.
  // If it ends up false, *err_code will indicate either success or non-fatal error.

  m_async_worker.post([&]()
  {
    // We are in thread W.

    assert((m_state == State::S_PEER)
           && "mdt is non-null, yet state is not PEER, even though PEER is an end state?  Bug.");

    if (Base::hosed())
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open request send-request stage preempted "
                       "by earlier session-hosed error.  No-op.");
      sync_error_but_do_not_emit = true;
      return;
    }
    // else if (!Base::hosed()): We can attempt sync_request() (which could hose us itself).

    auto& open_channel_req = *(reinterpret_cast<Master_channel_req*>(mdt.get()));

    // @todo Maybe treat it same as null mdt at open_channel() start?  Currently we promise undefined behavior:
    assert(open_channel_req.m_req_msg.body_root()->isOpenChannelToServerReq()
           && "Did you pass NULL-state-mdt_builder() result to open_channel()?  Use PEER-state one instead.");

    /* Now simply issue the open-channel request prepared already via `mdt` and synchronously
     * await response, or error, or timeout.  struc::Channel::sync_request() is tailor-made for this. */
    const auto open_channel_rsp = m_master_channel->sync_request(open_channel_req.m_req_msg, nullptr,
                                                                 Base::S_OPEN_CHANNEL_TIMEOUT, err_code);

    if ((!open_channel_rsp) && (!*err_code))
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open request failed at the send-request stage "
                       "(due to incoming-direction error which shall hose session around this time).  No-op.");
      sync_error_but_do_not_emit = true;
      return;
    }
    // else if (*err_code || open_channel_rsp)

    if (*err_code) // && !open_channel_rsp)
    {
      assert((!open_channel_rsp) && "struc::Channel::sync_request() must emit no Error_code if it gets response.");

      if (*err_code == transport::error::Code::S_TIMEOUT)
      {
        FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open active request: It timed out.  It may "
                         "complete in the background still, but when it does any result shall be ignored.  "
                         "Emitting (non-session-hosing) error.");

        *err_code = error::Code::S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT;
        // Non-session hosing error will be synchronously emitted; *this will continue (as promised).
        return;
      }
      // else if (*err_code truthy but not timeout): Session got hosed.

      /* As promised, open_channel() hosing the session shall be reported asynchronously like all errors;
       * while we will merely return false. */
      FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open request failed at the sync-request stage, "
                       "meaning either the nb-send failed, or in-error detected on channel while waiting for "
                       "response (new error: [" << *err_code << "] [" << err_code->message() << "]).  Hosing "
                       "session (emit to session on-error handler); will synchronously return failure to "
                       "open channel.");
      Base::hose(*err_code);
      sync_error_but_do_not_emit = true; // err_code is ignored.
      return;
    }
    // else if (!*err_code): sync_request() succeeded all the way.
    assert(open_channel_rsp);

    FLOW_LOG_TRACE("Client session [" << *this << "]: Channel open request: Request message sent to server "
                   "sans error, and response received in time.");

    const auto root = open_channel_rsp->body_root().getOpenChannelToServerRsp();
    switch (root.getOpenChannelResult())
    {
    case OpenChannelResult::ACCEPTED:
      assert(!*err_code);
      break;
    case OpenChannelResult::REJECTED_PASSIVE_OPEN:
      *err_code = error::Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN;
      break;
    case OpenChannelResult::REJECTED_RESOURCE_UNAVAILABLE:
      *err_code = error::Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE;
      break;
    case OpenChannelResult::END_SENTINEL:
      assert(false);
    } // Compiler should catch missing enum value.

    if (*err_code)
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Channel open active request: Response received but "
                       "indicates remote peer (server) refused/was unable to open channel for us; emitting "
                       "[" << *err_code << "] [" << err_code->message() << "] with explanation from remote "
                       "peer.  This will not hose session on our end.");
      // Note: all of the above are advertised as non-session-hosing.
      return;
    }
    // else: Cool! We can open our side of each channel component based on the stuff server prepared and sent to us.

    Native_handle local_hndl_or_null = open_channel_rsp->native_handle_or_null();
    const auto mq_name_c2s_or_none = Shared_name::ct(string(root.getClientToServerMqAbsNameOrEmpty()));
    const auto mq_name_s2c_or_none = Shared_name::ct(string(root.getServerToClientMqAbsNameOrEmpty()));

    create_channel_obj(mq_name_c2s_or_none, mq_name_s2c_or_none, std::move(local_hndl_or_null),
                       target_channel, true, // true => active.
                       err_code);
    // *err_code is either success or failure, and target_channel is either blank or a PEER-state Channel.

    assert((!*err_code)
           && "As of this writing the only path to this is inside the MQ-related guts of Channel creation (if "
                "MQs are even configured), namely because something in Blob_stream_mq_base_impl indicates "
                "there'd be no more than 1 MQ receiver or 1 MQ sender for a given MQ (c2s or s2c) "
                "and that is *after* server was able to get through its side of the same thing not to mention "
                "create the MQs in the first place.  We are not supposed to emit REJECTED_RESOURCE_UNAVAILABLE "
                "since this isn't even resource acquisition but merely hooking up to the resource acquired "
                "by server already.  So it is very strange no-man's-land; we abort.  @todo Reconsider.");

    FLOW_LOG_INFO("Client session [" << *this << "]: Channel open active request: Succeeded in time yielding "
                  "new channel [" << *target_channel << "].");
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); // m_async_worker.post()

  // It all logged sufficiently.  Just return the results.

  if (sync_error_but_do_not_emit)
  {
    err_code->clear();
    return false;
  }
  // else

  return true; // *err_code may be truthy or falsy.
} // Client_session_impl::open_channel()

TEMPLATE_CLI_SESSION_IMPL
bool CLASS_CLI_SESSION_IMPL::open_channel(Channel_obj* target_channel, Error_code* err_code)
{
  // We are in thread U.
  return open_channel(target_channel, mdt_builder(), err_code);
}

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::on_master_channel_open_channel_req
       (typename Master_structured_channel::Msg_in_ptr&& open_channel_req)
{
  using schema::detail::OpenChannelResult;
  using flow::util::ostream_op_string;
  using boost::shared_ptr;
  using std::string;

  // We are in thread W.

  if (Base::hosed())
  {
    /* Another thread-W task hosed the session.  It shouldn't be due to m_master_channel itself firing error,
     * as then we shouldn't be called, but maybe our own logic found some session-hosing condition in the meantime. */
    return;
  }
  // else if (!Base::hosed()): Our time to shine.  We may make Base::hosed() true ourselves, in peace, below.

  auto open_channel_rsp = m_master_channel->create_msg();
  OpenChannelResult open_channel_result = OpenChannelResult::ACCEPTED;
  Channel_obj opened_channel;

  const auto& on_passive_open_func = Base::on_passive_open_channel_func_or_empty();
  if (on_passive_open_func.empty())
  {
    open_channel_result = OpenChannelResult::REJECTED_PASSIVE_OPEN;
  }
  else // if (passive opens enabled)
  {
    const auto root = open_channel_req->body_root().getOpenChannelToClientReq();

    // Cool! We can open our side of each channel component based on the stuff server prepared and sent to us.

    auto local_hndl_or_null = open_channel_req->native_handle_or_null();
    const auto mq_name_c2s_or_none = Shared_name::ct(string(root.getClientToServerMqAbsNameOrEmpty()));
    const auto mq_name_s2c_or_none = Shared_name::ct(string(root.getServerToClientMqAbsNameOrEmpty()));

    Error_code err_code;
    create_channel_obj(mq_name_c2s_or_none, mq_name_s2c_or_none, std::move(local_hndl_or_null),
                       &opened_channel, false, // false => passive.
                       &err_code);
    // err_code is either success or failure, and opened_channel is either blank or a PEER-state Channel.

    assert((!err_code)
           && "As of this writing the only path to this is inside the MQ-related guts of Channel creation (if "
                "MQs are even configured), namely because something in Blob_stream_mq_base_impl indicates "
                "there'd be no more than 1 MQ receiver or 1 MQ sender for a given MQ (c2s or s2c) "
                "and that is *after* server was able to get through its side of the same thing not to mention "
                "create the MQs in the first place.  We are not supposed to emit REJECTED_RESOURCE_UNAVAILABLE "
                "since this isn't even resource acquisition but merely hooking up to the resource acquired "
                "by server already.  So it is very strange no-man's-land; we abort.  @todo Reconsider.");

    // Success!
  } // else if (passive opens enabled)

  auto open_channel_rsp_root = open_channel_rsp.body_root()->initOpenChannelToClientRsp();
  const auto open_channel_req_saved = open_channel_req.get(); // It'll get move()d away below possibly; just save it.
  open_channel_rsp_root.setOpenChannelResult(open_channel_result);
  Mdt_reader_ptr mdt; // Null for now; set inside below in the success case.

  if (open_channel_result == OpenChannelResult::ACCEPTED)
  {
    /* open_channel_rsp is ready to go.  That's for us to send.
     * Locally we need to emit opened_channel; that's ready too.
     * Locally, lastly, we need to emit the channel-open metadata from the open-channel request.  Take care of that
     * now.  We just give them the entire in-message, invisibly, by supplying a shared_ptr<> alias to a little
     * struct that stores the actual in-message and a reader into its metadata sub-field.  Once they drop their alias
     * shared_ptr, the struct will deleted; and since m_req inside it has ref-count=1, that'll also be dropped,
     * and the in-message can finally RIP.
     *
     * By the way, this is pretty similar to Master_channel_req (see its doc header) on the open_channel() end;
     * but it's simpler, as no one will ever need to get to the m_req itself anymore -- it just needs to
     * stay alive, as the Reader is backed by its bits. */
    struct Req_and_mdt_reader
    {
      typename Mdt_reader_ptr::element_type m_mdt_reader;
      typename Master_structured_channel::Msg_in_ptr m_req;
    };
    shared_ptr<Req_and_mdt_reader> req_and_mdt(new Req_and_mdt_reader
                                                     { open_channel_req->body_root()
                                                         .getOpenChannelToClientReq().getMetadata(),
                                                        std::move(open_channel_req) });
    mdt = Mdt_reader_ptr(std::move(req_and_mdt), &req_and_mdt->m_mdt_reader);
  } // if (open_channel_result == ACCEPTED)

  Error_code err_code;
  if (!m_master_channel->send(open_channel_rsp, open_channel_req_saved, &err_code))
  {
    return; // It'll fire on_master_channel_error(); not our problem.
  }
  // else
  if (err_code)
  {
    FLOW_LOG_WARNING("Client session [" << *this << "]: Passive open-channel: send() of OpenChannelToClientRsp "
                     "failed (details above presumably); this hoses the session.  Emitting to user handler.");
    Base::hose(err_code);
    return;
  } // if (err_code on send())
  // else

  if (open_channel_result != OpenChannelResult::ACCEPTED)
  {
    // Cool.  Well, from our point of view it's cool; the user has some stuff to figure out on their app protocol level.
    FLOW_LOG_WARNING("Client session [" << *this << "]: Passive open-channel: We have rejected passive-open request "
                     "(reason explained above hopefully).  Response indicating this is sent.  Done.");
    return;
  }
  // else

  // Great!
  FLOW_LOG_INFO("Client session [" << *this << "]: Passive open-channel: Successful on this side!  Response sent; "
                "remote peer (server) has already established its peer object before sending the request.  "
                "We emit local peer Channel object [" << opened_channel << "], plus the channel-open metadata, "
                "to user via passive-channel-open handler.");
  on_passive_open_func(std::move(opened_channel), std::move(mdt));
  FLOW_LOG_TRACE("Handler finished.");
} // Client_session_impl::on_master_channel_open_channel_req()

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::create_channel_obj(const Shared_name& mq_name_c2s_or_none,
                                                const Shared_name& mq_name_s2c_or_none,
                                                util::Native_handle&& local_hndl_or_null,
                                                Channel_obj* opened_channel_ptr,
                                                bool active_else_passive,
                                                Error_code* err_code_ptr)
{
  using transport::sync_io::Native_socket_stream;
  using transport::Socket_stream_channel;
  using transport::Mqs_channel;
  using transport::Mqs_socket_stream_channel;
  using transport::Socket_stream_channel_of_blobs;
  using flow::util::ostream_op_string;

  assert(err_code_ptr);
  assert((!*err_code_ptr) && "Should start with success code.");

  auto& opened_channel = *opened_channel_ptr;
  const auto nickname = active_else_passive ? ostream_op_string("active", ++m_last_actively_opened_channel_id)
                                            : ostream_op_string("passive", ++m_last_passively_opened_channel_id);

  assert((S_SOCKET_STREAM_ENABLED == (!local_hndl_or_null.null()))
         && "We and server peer agreed on has-socket-stream channel config, yet they failed to send/not-send us "
              "a native socket handle accordingly.  Other side misbehaved terribly.  @todo Consider emitting error.");
  assert((((!S_MQS_ENABLED) == mq_name_c2s_or_none.empty())
          &&
          ((!S_MQS_ENABLED) == mq_name_c2s_or_none.empty()))
         && "We and server peer agreed on MQ-type channel config, yet they failed to send/not-send us "
              "the 2 names accordingly.  Other side misbehaved terribly.  @todo Consider emitting error.");

  if constexpr(S_MQS_ENABLED)
  {
    Error_code err_code1;
    Error_code err_code2;
    Persistent_mq_handle_from_cfg mq_c2s(get_logger(), mq_name_c2s_or_none, util::OPEN_ONLY, &err_code1);
    Persistent_mq_handle_from_cfg mq_s2c(get_logger(), mq_name_s2c_or_none, util::OPEN_ONLY, &err_code2);

    /* Corner case: Server already created underlying MQs, apparently successfully, so on unlikely failure
     * to create the MQ-based channel below it is up to the server to not-leak those underlying MQs.
     * Omitting further discussion, but indeed server side does take care of this non-trivial task. */

    /* @todo Corner case: What to do with the (FD) local_hndl_or_null, if not .null(), on (MQ-related) failure?
     * Maybe we should ::close(local_hndl_or_null.m_native_handle)?  It is quite a corner case; it came from
     * server-side connect_pair(), and they presumably still may hold the other peer FD, and it is connected?
     * It is hard to reason about, as not only did all that already succeed on server end, but *their* MQ stuff
     * succeeded too -- yet ours failed, and we aren't even creating the MQs, just hooking up to them.
     * This is not very high-priority but still; it might be kinda/sorta an FD leak or something.  What would
     * it mean to ::close() our transmitted "copy" of one of the peer FDs?  Investigate. */

    if ((*err_code_ptr = (err_code1 ? err_code1 : err_code2)))
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Open-channel (active? = [" << active_else_passive << "]): "
                       "Request or response received and indicates remote peer (server) generated channel "
                       "resources for us; but we failed to open our side of 1 or both MQs (see above); "
                       "emitting the first one to fail of the 2 "
                       "([" << *err_code_ptr << "] [" << err_code_ptr->message() << "]).");
      return;
    }
    // else: cool.  Last thing, possibly:

    if constexpr(S_SOCKET_STREAM_ENABLED)
    {
      static_assert(std::is_same_v<Channel_obj,
                                   Mqs_socket_stream_channel<true, Persistent_mq_handle_from_cfg>>, "Sanity check.");
      assert((!local_hndl_or_null.null()) && "This has been ascertained 20 ways by now....");

      opened_channel = Channel_obj(get_logger(), nickname, std::move(mq_c2s), std::move(mq_s2c),
                                   Native_socket_stream(get_logger(), nickname,
                                                        std::move(local_hndl_or_null)),
                                   err_code_ptr);
    }
    else // if constexpr(!S_SOCKET_STREAM_ENABLED)
    {
      static_assert(std::is_same_v<Channel_obj,
                                   Mqs_channel<true, Persistent_mq_handle_from_cfg>>, "Sanity check.");
      assert(local_hndl_or_null.null() && "This has been ascertained 20 ways by now....");

      opened_channel = Channel_obj(get_logger(), nickname, std::move(mq_c2s), std::move(mq_s2c),
                                   err_code_ptr);
    } // else if constexpr(!S_SOCKET_STREAM_ENABLED)

    if (*err_code_ptr)
    {
      FLOW_LOG_WARNING("Client session [" << *this << "]: Open-channel (active? = [" << active_else_passive << "]): "
                       "Request or response received and indicates remote peer (server) generated channel "
                       "resources for us; but we failed to create the channel due to something MQ-related, "
                       "even though MQ handles themselves opened fine; emitting "
                       "([" << *err_code_ptr << "] [" << err_code_ptr->message() << "]).");
    }
    // else { All good.  Still done (falsy *err_code_ptr). }
  } // if constexpr(S_MQS_ENABLED)
  else // if constexpr(!S_MQS_ENABLED)
  {
    static_assert(S_SOCKET_STREAM_ENABLED, "Either MQs or socket stream or both must be configured on somehow.");
    assert((!local_hndl_or_null.null()) && "One of the pipes must enabled; runtime stuff asserted above already.");

    if constexpr(S_TRANSMIT_NATIVE_HANDLES)
    {
      static_assert(std::is_same_v<Channel_obj, Socket_stream_channel<true>>, "Sanity check.");
      opened_channel
        = Channel_obj(get_logger(), nickname,
                      Native_socket_stream(get_logger(), nickname, std::move(local_hndl_or_null)));
    }
    else
    {
      static_assert(std::is_same_v<Channel_obj, Socket_stream_channel_of_blobs<true>>, "Sanity check.");
      opened_channel
        = Channel_obj(get_logger(), nickname,
                      Native_socket_stream(get_logger(), nickname, std::move(local_hndl_or_null)));
    } // else if constexpr(!S_TRANSMIT_NATIVE_HANDLES)
  } // if constexpr(!S_MQS_ENABLED)
} // Client_session_impl::create_channel_obj()

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::cancel_peer_state_to_null()
{
  // We are in thread W.
  assert(m_async_worker.in_thread()
         && "By contract this internal/protected API must be invoked from our own thread W (from "
               "on-async-connect-success handler).");

  FLOW_LOG_WARNING("Client session [" << *this << "]: While the vanilla portion of async-connect succeeded, "
                   "the post-vanilla post-processing portion then synchronously detected a problem; hence we are "
                   "returning to NULL state after all.");

  // Do what we would have done just before calling on_done_func(<failure>) (see on_master_channel_log_in_rsp()).
  m_master_channel.reset();
  m_state = State::S_NULL;
}

TEMPLATE_CLI_SESSION_IMPL
template<typename Task_err>
typename CLASS_CLI_SESSION_IMPL::Master_structured_channel*
  CLASS_CLI_SESSION_IMPL::cancel_peer_state_to_connecting(Task_err&& on_done_func)
{
  // We are in thread W.
  assert(m_async_worker.in_thread()
         && "By contract this internal/protected API must be invoked from our own thread W (from "
               "on-async-connect-success handler).");

  FLOW_LOG_INFO("Client session [" << *this << "]: While the vanilla portion of async-connect succeeded, "
                "the post-vanilla post-processing portion requires further asynchronous work (e.g., to acquire "
                "additional resources for internal SHM-support purposes); hence we are "
                "returning to CONNECTING state.");

  m_state = State::S_CONNECTING;
  m_conn_on_done_func_or_empty = std::move(on_done_func); // Again: May be invoked from thread W or thread U (dtor).

  return m_master_channel.get();
}

TEMPLATE_CLI_SESSION_IMPL
void CLASS_CLI_SESSION_IMPL::complete_async_connect_after_canceling_peer_state(const Error_code& err_code_or_success)
{
  // We are in thread W.
  assert(m_async_worker.in_thread()
         && "By contract this internal/protected API must be invoked from our own thread W.");

  assert((!Base::hosed()) && "Per contract caller should avoid calling us if already hosed.");

  if (err_code_or_success)
  {
    FLOW_LOG_WARNING("Client session [" << *this << "]: Post-vanilla-success portion of async-connect failed (details "
                     "surely above).  Executing handler.");

    // Similarly to cancel_peer_state_to_null():
    m_master_channel.reset();
    m_state = State::S_NULL;
  }
  else
  {
    FLOW_LOG_INFO("Client session [" << *this << "]: Post-vanilla-success portion of async-connect also succeeded.  "
                  "Will go to PEER state and report to user via on-async-connect handler.");
    m_state = State::S_PEER;

    // See Session_base::Graceful_finisher doc header for all the background ever.
    if constexpr(S_GRACEFUL_FINISH_REQUIRED)
    {
      m_graceful_finisher.emplace(get_logger(), this, &m_async_worker, m_master_channel.get());
    }
  }

  invoke_conn_on_done_func(err_code_or_success);
} // Client_session_impl::complete_async_connect_after_canceling_peer_state()

TEMPLATE_CLI_SESSION_IMPL
flow::async::Single_thread_task_loop* CLASS_CLI_SESSION_IMPL::async_worker()
{
  return &m_async_worker;
}

TEMPLATE_CLI_SESSION_IMPL
const typename CLASS_CLI_SESSION_IMPL::Master_structured_channel& CLASS_CLI_SESSION_IMPL::master_channel_const() const
{
  return *m_master_channel;
}

TEMPLATE_CLI_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_CLI_SESSION_IMPL& val)
{
  /* Let's print only really key stuff here; various internal code likes to ostream<< *this session (`val` to us),
   * and we don't want to be treating stuff here as immutable or safe to access, even though it might change
   * concurrently.  So we intentionally just print the Client_app and the Server_app (which are themselves immutable,
   * but the ref/ptr to each inside `val` might not be).  Now, for Client_session, both ptrs/refs are indeed
   * immutable, so it's safe. */

  const auto cli_app_ptr = val.cli_app_ptr();
  assert(cli_app_ptr && "Client_session's Client_app ptr must be known from construction.");
  os << '[' << cli_app_ptr->m_name << "->" << val.m_srv_app_ref.m_name;
  if constexpr(CLASS_CLI_SESSION_IMPL::S_SHM_ENABLED)
  {
    os << " | shm_type=" << int(CLASS_CLI_SESSION_IMPL::S_SHM_TYPE);
  }
  return os << "]@" << static_cast<const void*>(&val);
}

#undef CLASS_CLI_SESSION_IMPL
#undef TEMPLATE_CLI_SESSION_IMPL

} // namespace ipc::session
