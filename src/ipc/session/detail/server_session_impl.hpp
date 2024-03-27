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
#include "ipc/transport/protocol_negotiator.hpp"

namespace ipc::session
{

// Types.

/**
 * Internal, non-movable pImpl-lite implementation of Server_session_mv class template.
 * In and of itself it would have been directly and publicly usable; however Server_session_mv adds move semantics.
 *
 * @see All discussion of the public (and `protected`, to be exposed `public`ly by Server_session_dtl) API is in
 *      Server_session_mv doc header; that class template forwards to this
 *      one.  All discussion of pImpl-lite-related notions is also there.  See that doc header first please.  Then come
 *      back here.
 *
 * Impl design
 * -----------
 * Generally, I (ygoldfel) recommend looking through and understanding Client_session_impl first.  It is similar but
 * somewhat more complex, and once it is understood, you'll be able to understand Server_session_impl easily.  Plus
 * its logic is complementary to a `*this`, as the two directly interoperate via a couple of IPC forms.  The below
 * discussion does assume one has read/understood Client_session_impl doc header.
 *
 * Thread U represents all threads other than thread W: since the relevant methods are to be called by the user
 * sans concurrency, those threads as a collection can be thought of as one thread.
 *
 * Thread W is the async worker thread where most work is done; this helps us entirely avoid mutexes.
 * async_accept_log_in() is called exactly once, internally by Session_server; and subsequently
 * open_channel() is fairly infrequently called.  So it's not necessary to avoid
 * latency by doing work concurrently in threads U and W.  So we keep it simple by posting most stuff onto thread W.
 *
 * The state machine of Server_session_impl is simpler than Client_session_impl.  The asymmetry is, firstly, due to the
 * client-server setup: Session_server is the one, on the server end, that awaits the initial
 * transport::Native_socket_stream connection.  Once a PEER-state `Native_socket_stream` is ready, only then
 * does Session_server create a Server_session_dtl (which is a `public` facade to Server_session_mv, which
 * is a movable `unique_ptr` wrapper around this Server_session_impl).  So `*this`, unlike a Client_session_impl,
 * does not need to connect a socket.  Next, Server_session_impl just needs to complete one async step, which is
 * in practice immediately (and, by contract, only once) invoked: async_accept_log_in().  At this stage user does
 * not have the Server_session_mv yet, so we're still in internal-code land.  (By contrast Client_session is
 * created directly by the user and is immediately publicly available as a stand-alone object.)  async_accept_log_in()
 * immediately upgrades the `Native_socket_stream` to a transport::Channel and then immediately upgrades that
 * to a transport::struc::Channel, saving it to #m_master_channel (all synchronously).  Lastly is the actual async work:
 * awaiting the log-in request from Client_session.  Once that is received, we respond via #m_master_channel,
 * either with log-in success or failure.  Assuming success, we invoke the (internal) async_accept_log_in()
 * handler; on failure same thing but with a failure #Error_code.
 *
 * ### Sidebar: Init-channels ###
 * Update (init-channels feature has been added): The preceding paragraph holds fully if and only if:
 *   - client (in its log-in request message, `LogInReq`) requested 0 init-channels to be opened on its behalf; and
 *   - server (via the user args from Session_server::async_accept() forwarded to async_accept_log_in()) requested
 *     0 init-channels to be opened on local user's behalf.
 *
 * However, if the sum of these 2 numbers (as determined upon successful receipt on `LogInReq`, and assuming
 * everything else was also successful, up-to-and-including sending successful `LogInRsp`) is N=1+, then before
 * async_accept_log_in() can reach almost-PEER state (and fire completion handler), there is 1 more async-step:
 *   - (send `LogInRsp` as explained);
 *   - (prep) synchronously prepare the N=1+ init-channels (exactly as would occur
 *     if user invoked open_channel() N times in PEER state);
 *   - (prep) sync-send N `OpenChannelToClientReq` messages with names/handles for each of the N init-channels
 *     (exactly as would occur on N open_channel() calls in PEER state);
 *   - async: await a *single* `OpenChannelToClientRsp` response, indicating the last of the N channels has been
 *     accepted, and client is now in PEER state and ready for passive-opens (i.e., we can enter PEER-state
 *     which enabled open_channel() by the local user).
 *
 * Why this last async step?  Couldn't we just do the `send()`s and go to almost-PEER state immediately?
 * Almost: but if we did that, then if the user immediately issued init_handlers() and an open_channel(),
 * then the opposing Client_session_impl might not, technically, be quick enough to have moved to its own PEER state;
 * then its `OpenChannelToClientReq` handler from the pre-PEER state (CONNECTING state) might be triggered;
 * this would cause... nothing good.  However the ACK from Client_session_impl is sent only just before
 * entering PEER state, at which point it is safe for us to issue open_channel()s.
 *
 * ### Back to general impl discussion ###
 * Our async_accept_log_in() is analogous/complementary to Client_session_impl::async_connect(), but the diff is that
 * the user only gets access to a `*this` upon success of this async op; and never gets access upon failure
 * at all.  So that's another source of asymmetry versus Client_session_impl.  Thus:
 *   - No need to guard against async_accept_log_in() call while the log-in is outstanding.
 *     - (As of *this* writing Client_session_impl only uses async_connect() internally and does not expose it,
 *       so it's not an issue there either; but that would change, if it became network-enabled at some level.)
 *   - No need to guard against other API calls, notably open_channel(), before (almost-)PEER state is achieved.
 *     (User only gets access to `*this`, once `*this` is in (almost-)PEER state.)
 *
 * Therefore there's no need to track an `m_state` the way Client_session_impl does.  If #m_log_in_on_done_func
 * is non-empty, then async_accept_log_in() is outstanding (analogous to Client_session_impl CONNECTING state).
 * Otherwise see the "almost-PEER" section just below.
 *
 * ### Almost-PEER state impl ###
 * There is a minor wrinkle in Server_session_mv (and thus Server_session_impl).  Once the user obtains
 * the Server_session_mv, it is in the so-called *almost-PEER* state.  That is because it needs 1-2 more pieces of
 * info to be set before it is formally in PEER state as specified by Session concept.  Namely its
 * on-error handler must be set (mandatory), as must its on-passive-open handler (optional; in that leaving it
 * unspecified is allowed and means passive-opens are disabled on this side).  To do so the *user*
 * shall call init_handlers().  Until this is done, open_channel() and other public PEER-state APIs shall
 * no-op and return sentinel/`false`.
 *
 * There is a subtlety in how this are-handlers-set check is performed.  The crux of it is that #m_master_channel
 * can report an error at any time starting with async_accept_log_in() (where it is first created), and what to
 * do is quite different in each case:
 *   - Before public availability to user (async log-in ongoing).
 *     - Contingency: report failure of async_accept_log_in(), via its on-done handler.
 *   - After that but in almost-PEER state (on-error handler not yet set by user).
 *     - Contingency: well, we *want* to report it via on-error handler... but it's not set yet.
 *       So memorize it in special member #m_pre_init_err_code.  Then in init_handlers() immediately fire off
 *       the freshly-available on-error handler with that #Error_code.
 *   - In PEER state (the handler has been set).
 *     - Contingency: yay, forward the #m_master_channel-reported #Error_code to the on-error
 *       handler.
 *
 * Because the #m_master_channel error handler is relevant across all of the above, and it is executed in thread W
 * for reasons explained above (avoiding mutexes, etc.), to avoid concurrency difficulties the are-handlers-set
 * check, and the assignment of the handlers, is also done in thread W.  Hence, like most code in Server_session_impl,
 * init_handlers() posts all its work onto thread W.  All public APIs, even simple ones like session_token(),
 * do the same, and that thread-W work always begins by ensuring handlers_are_set().
 *
 * Once PEER state is achieved, to the user `*this` is (just like Client_session_impl) an impl of Session concept.
 * Internally it's somewhat different.
 *
 * ### PEER state impl ###
 * Here our algorithm is complementary to the PEER state algorithm in Client_session_impl.  Namely we expect
 * passive-opens via an appropriate transport::struc::Channel::expect_msgs(); and we allow active-opens via
 * open_channel(). The details of how these work is best understood just by reading that code inline.  All in
 * all, publicly it's just like Client_session_impl; but internally it is its complement and in fact has
 * different (complementary) responsibilities.  Most notably, regarding active-opens and passive-opens:
 *   - Server_session_impl is the one always internally responsible for acquiring the resources needed to
 *     establish the channel.  Namely:
 *     - If #S_MQS_ENABLED: it *creates* the underlying MQs.  This involves choosing a distinct-enough name for
 *       each of the 2 MQs (one in each direction, client->server, server->client).  These names shall be
 *       sent-over, via #m_master_channel, to the Client_session_impl.
 *     - If #S_SOCKET_STREAM_ENABLED: it *creates* the pre-connected socket-pair, 1/2 of which (a `Native_handle`)
 *       shall be sent-over, via #m_master_channel, to the Client_session_impl.
 *   - Hence:
 *     - An active-open (open_channel()) sends over the references/handles to the above resource(s) in the
 *       open-channel-to-client request out-message via #m_master_channel.  It then ensures the client
 *       replies with an OK (or failure) response in-message.  open_channel() thus succeeds or fails.
 *     - A passive-open (on_master_channel_open_channel_req()) involves receiving an
 *       open-channel-to-server request in-message via #m_master_channel.  It then acquires the above resource(s)
 *       and sends over the refs/handles to said resource(s) in our OK response to that request and invokes
 *       the on-passive-open handler locally.
 *
 * Client_session_impl active-opens and passive-opens act in a complementary fashion, never acquiring these resources
 * but knowing what to expect.
 *
 * Otherwise open_channel() is set up similarly to Client_session_impl::open_channel(), namely the
 * non-blocking/synchronous facade with async-with-timeout internals.  See Client_session_impl doc header for
 * brief discussion.
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         See #Server_session counterpart.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See #Server_session counterpart.
 * @tparam Mdt_payload
 *         See #Server_session counterpart.
 * @tparam S_SHM_TYPE_OR_NONE
 *         See common.capnp `ShmType` capnp-`enum` definition.
 * @tparam S_SHM_MAX_HNDL_SZ
 *         Ignored if `S_SHM_TYPE_OR_NONE` indicates we are not SHM-enabled, this otherwise indicates the max
 *         size of a SHM handle's blob serialization.  If ignored it must be set to 0 (convention).
 * @tparam S_GRACEFUL_FINISH_REQUIRED_V
 *         `true` if and only if Session_base::Graceful_finisher must be used.
 *         See its doc header for explanation when that would be the case.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload,
         schema::ShmType S_SHM_TYPE_OR_NONE, size_t S_SHM_MAX_HNDL_SZ, bool S_GRACEFUL_FINISH_REQUIRED_V>
class Server_session_impl :
  public Session_base<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>,
  public flow::log::Log_context
{
public:
  // Types.

  /// Short-hand for base type.
  using Base = Session_base<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>;

  /// Short-hand for Session_base super-class.
  using Session_base_obj = Base;

  /// See Server_session_mv counterpart.
  using Channel_obj = typename Base::Channel_obj;

  /// See Session_mv counterpart.
  using Channels = typename Base::Channels;

  /// See Server_session_mv counterpart.
  using Mdt_payload_obj = typename Base::Mdt_payload_obj;

  /// See Server_session_mv counterpart.
  using Mdt_reader_ptr = typename Base::Mdt_reader_ptr;

  /// See Server_session_mv counterpart.
  using Mdt_builder = typename Base::Mdt_builder;

  /// See Server_session_mv counterpart.
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

  static_assert(S_SHM_ENABLED || (S_SHM_MAX_HNDL_SZ == 0),
                "S_SHM_MAX_HNDL_SZ is ignored if SHM disabled; and must be set to 0 by required convention.");

  /// See Server_session_mv counterpart.
  static constexpr bool S_MQS_ENABLED = Base::S_MQS_ENABLED;

  /// See Server_session_mv counterpart.
  static constexpr bool S_SOCKET_STREAM_ENABLED = Base::S_SOCKET_STREAM_ENABLED;

  /// Short-hand for template parameter knob `S_GRACEFUL_FINISH_REQUIRED_V`: see class template doc header.
  static constexpr bool S_GRACEFUL_FINISH_REQUIRED = S_GRACEFUL_FINISH_REQUIRED_V;

  // Constructors/destructor.

  /**
   * For use by internal user Session_server: See Server_session_mv counterpart.
   *
   * @param logger_ptr
   *        See Server_session_mv counterpart.
   * @param srv_app_ref
   *        See Server_session_mv counterpart.
   * @param master_channel_sock_stm
   *        See Server_session_mv counterpart.
   */
  explicit Server_session_impl(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                               transport::sync_io::Native_socket_stream&& master_channel_sock_stm);

  /// See Server_session_mv counterpart.
  ~Server_session_impl();

  // Methods.

  /**
   * For use by internal user Session_server: See Server_session_mv counterpart.
   *
   * @param srv
   *        See Server_session_mv counterpart.
   * @param init_channels_by_srv_req
   *        See Server_session_mv counterpart.
   * @param mdt_from_cli_or_null
   *        See Server_session_mv counterpart.
   * @param init_channels_by_cli_req
   *        See Server_session_mv counterpart.
   * @param cli_app_lookup_func
   *        See Server_session_mv counterpart.
   * @param cli_namespace_func
   *        See Server_session_mv counterpart.
   * @param pre_rsp_setup_func
   *        See Server_session_mv counterpart.
   * @param n_init_channels_by_srv_req_func
   *        See Server_session_mv counterpart.
   * @param mdt_load_func
   *        See Server_session_mv counterpart.
   * @param on_done_func
   *        See Server_session_mv counterpart.
   */
  template<typename Session_server_impl_t,
           typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept_log_in(Session_server_impl_t* srv,
                           Channels* init_channels_by_srv_req,
                           Mdt_reader_ptr* mdt_from_cli_or_null,
                           Channels* init_channels_by_cli_req,
                           Cli_app_lookup_func&& cli_app_lookup_func, Cli_namespace_func&& cli_namespace_func,
                           Pre_rsp_setup_func&& pre_rsp_setup_func,
                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                           Mdt_load_func&& mdt_load_func,
                           Task_err&& on_done_func);

  /**
   * See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  const Client_app* client_app() const;

  /**
   * See Server_session_mv counterpart.
   *
   * @param on_err_func_arg
   *        See Server_session_mv counterpart.
   * @param on_passive_open_channel_func_arg
   *        See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  template<typename Task_err, typename On_passive_open_channel_handler>
  bool init_handlers(Task_err&& on_err_func_arg, On_passive_open_channel_handler&& on_passive_open_channel_func_arg);

  /**
   * See Server_session_mv counterpart.
   *
   * @param on_err_func_arg
   *        See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  template<typename Task_err>
  bool init_handlers(Task_err&& on_err_func_arg);

  /**
   * See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  Mdt_builder_ptr mdt_builder();

  /**
   * See Server_session_mv counterpart.
   *
   * @param target_channel
   *        See Server_session_mv counterpart.
   * @param mdt
   *        See Server_session_mv counterpart.
   * @param err_code
   *        See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  bool open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt, Error_code* err_code = 0);

  /**
   * See Server_session_mv counterpart.
   *
   * @param target_channel
   *        See Server_session_mv counterpart.
   * @param err_code
   *        See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  bool open_channel(Channel_obj* target_channel, Error_code* err_code = 0);

  /**
   * See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  const Session_token& session_token() const;

  /**
   * See Server_session_mv counterpart.
   * @return See Server_session_mv counterpart.
   */
  const Base& base() const;

protected:
  // Types.

  /// See Session_base.
  using Master_structured_channel = typename Base::Master_structured_channel;

  // Methods.

  /**
   * Utility for sub-classes: ensures that `task()` is invoked near the end of `*this` dtor's execution, from thread
   * U, after thread W is joined, and any async_accept_log_in() on-done handler has fired with operation-aborted code.
   * It requires being invoked from thread W.  (Behavior is undefined otherwise; assertion may trip.)
   *
   * The value it adds: A long story best told by specific example.  See the original use case which is
   * in shm::classic::Server_session_impl::async_accept_log_in(); it sets up certain SHM cleanup steps to occur,
   * when the session ends.
   *
   * As of this writing this can be invoked 2+ times if desired, but really the spirit of it is to either never
   * call it, or call it exactly once during a certain stage of async_accept_log_in()'s async portion.
   *
   * ### Watch out! ###
   * At the time `task()` runs, the calling instance of the sub-class has been destroyed -- thus it is, e.g.,
   * usually wrong to capture your `this` in the `task` lambda, except for logging.
   * `get_logger()` and `get_log_component()` (which are immutable state) are still okay to use.
   *
   * @tparam Task
   *         Function object invoked as `void` with no args.
   * @param task
   *        `task()` shall execute before dtor returns.
   */
  template<typename Task>
  void sub_class_set_deinit_func(Task&& task);

  /**
   * Utility for sub-classes: provides ability to schedule or post tasks onto thread W.
   * @return See above.
   */
  flow::async::Single_thread_task_loop* async_worker();

  /**
   * Utility for sub-classes: provides ability to do work on the session master channel after our async_accept_log_in()
   * succeeds, but before the sub-classed wrapper of its on-done handler succeeds.
   *
   * This is pushing my (ygoldfel) (and from coding guide) suggestion to avoid unrestricted `protected` access
   * to data members.  It still "feels" okay but....  Hmmm.
   *
   * @return See above.
   */
  Master_structured_channel* master_channel();

  /**
   * Analogous to Client_session_impl::master_channel_const().  See that doc header.
   * @return See above.
   */
  const Master_structured_channel& master_channel_const() const;

  /// Analogous to Client_session_impl::dtor_async_worker_stop().  See that doc header.
  void dtor_async_worker_stop();

private:
  // Types.

  /// See Session_base.
  using Persistent_mq_handle_from_cfg = typename Base::Persistent_mq_handle_from_cfg;

  /// See Session_base.
  using On_passive_open_channel_func = typename Base::On_passive_open_channel_func;

  /// See Session_base.
  using Master_structured_channel_ptr = typename Base::Master_structured_channel_ptr;

  /**
   * An open-channel request out-message.  Together with that is stored a `Builder` into its metadata
   * sub-field as well as the already-prepared #Channel_obj, the local transport::Channel peer object in PEER state,
   * since on the server side it is our responsibility acquire the shared resources for both sides.
   *
   * The design here is identical to Client_session_impl::Open_channel_req; so see its doc header.  The only
   * difference is the addition of #m_opened_channel; see its doc header just below.
   */
  struct Open_channel_req
  {
    /// See Client_session_impl::Master_channel_req::m_mdt_builder.
    typename Mdt_builder_ptr::element_type m_mdt_builder;
    /// See Client_session_impl::Master_channel_req::m_req_msg.
    typename Master_structured_channel::Msg_out m_req_msg;

    /**
     * The transport::Channel, including the needed shared resources acquired, from
     * Server_session_impl::mdt_builder(); or a default-cted such object if that method failed to
     * acquire the needed resources (e.g., MQs).  To test for that failure simply check
     * transport::Channel::initialized().
     */
    Channel_obj m_opened_channel;
  }; // struct Open_channel_req

  /**
   * Short-hand for ref-counted pointer to Open_channel_req.
   * @see Client_session_impl::Open_channel_req doc header for info on the true nature of such `shared_ptr` as
   *      returned by us.
   */
  using Open_channel_req_ptr = boost::shared_ptr<Open_channel_req>;

  // Methods.

  /**
   * Core of both init_handlers() overloads, to be invoked in thread W.
   *
   * @param on_err_func_arg
   *        See init_handlers().  Concretely-typed version of that arg.
   * @param on_passive_open_channel_func_or_empty_arg
   *        See init_handlers().  Concretely-typed version of that arg; or `.empty()` one if the
   *        passive-opens-disabled overload was called.
   * @return See init_handlers().
   */
  bool init_handlers_impl(flow::async::Task_asio_err&& on_err_func_arg,
                          On_passive_open_channel_func&& on_passive_open_channel_func_or_empty_arg);

  /**
   * In thread W, returns whether init_handlers_impl() has yet executed.  This shall be checked in the thread-W
   * body of ~all APIs in this class.
   *
   * @return See above.
   */
  bool handlers_are_set() const;

  /**
   * In thread W, handler for #m_master_channel indicating incoming-direction channel-hosing error.
   * It is possible that #m_master_channel has been `.reset()` in the meantime, by seeing log-in failure
   * in on_master_channel_open_channel_req(), and no longer exists.
   *
   * @param err_code
   *        The non-success #Error_code from transport::struc::Channel.
   */
  void on_master_channel_error(const Error_code& err_code);

  /**
   * In thread W, handler for #m_master_channel receiving a passive-open (a/k/a open-channel-to-server) request.
   * If there is no issue with this request, and we're able to sync-send the open-channel response to that effect,
   * this shall fire on-passive-open handler, giving it a new #Channel_obj in PEER state + metadata.
   * If there is a problem, then it's not a session-hosing situation; local user is none-the-wiser; except that
   * if the `send()` reveals a *new* error, then it is a session-hosing situation, and local user is informed
   * via on-error handler.
   *
   * @param open_channel_req
   *        The request in-message from transport::struc::Channel.
   */
  void on_master_channel_open_channel_req(typename Master_structured_channel::Msg_in_ptr&& open_channel_req);

  /**
   * In thread W acquires the needed shared sources (MQs and/or `Native_handle` pair as of this writing) and creates
   * local #Channel_obj to emit to the user thus completing the channel-open on this side.  If active-open on our part,
   * we do this immediately in mdt_builder() -- before open_channel() even -- but before the
   * #Channel_obj can be emitted to the user (1) open_channel() must actually be called (obv) and (2) client
   * must respond OK to our open-channel-to-client request out-message.  If passive-open, we do this upon receiving
   * client's open-channel-to-server request, before replying with our OK.  Either way we are the ones acquiring the
   * resources, then we send names/refs to those resources to client.
   *
   * Returns `false` if and only if could not acquire resources.
   *
   * @param mq_name_c2s_or_none_ptr
   *        If #S_MQS_ENABLED this out-arg is the (non-empty) name of the client->server unidirectional MQ for the
   *        channel.  Else ignored.  Must be empty at entry or behavior undefined (assertion may trip).
   * @param mq_name_s2c_or_none_ptr
   *        Like the preceding but opposite-direction (server->client).
   * @param remote_hndl_or_null_ptr
   *        If #S_SOCKET_STREAM_ENABLED this out-arg is the pre-connected `Native_handle` to send to the other
   *        side; we keep the other end and shove it into the #Channel_obj out-arg.  Else ignored.
   *        Must be `.null()` or behavior undefined (assertion may trip).
   * @param opened_channel_ptr
   *        Target #Channel_obj we shall try to move-to PEER state.  It'll be left unmodified if
   *        we return `false`.
   * @param active_else_passive
   *        `true` if this is from open_channel(); `false` if from on_master_channel_open_channel_req().
   * @return `true` on success; `false` if a resource could not be acquired.
   */
  bool create_channel_and_resources(Shared_name* mq_name_c2s_or_none_ptr, Shared_name* mq_name_s2c_or_none_ptr,
                                    util::Native_handle* remote_hndl_or_null_ptr, Channel_obj* opened_channel_ptr,
                                    bool active_else_passive);

  /**
   * Helper for create_channel_and_resources(), invoked and compiled if and only if #S_MQS_ENABLED, that *creates*
   * the underlying client->server and server->client MQs and local handles thereto, and outputs these things
   * via out-args.  On failure undoes everything it was able to do and returns `false`; else returns `true`.
   *
   * @param mq_c2s
   *        On success this is the handle to newly creates client->server unidirectional MQ for the new channel.
   * @param mq_s2c
   *        Like the preceding but opposite-direction (server->client).
   * @param mq_name_c2s
   *        Name for `mq_c2s`.
   * @param mq_name_s2c
   *        Name for `mq_s2c`.
   * @param err_code
   *        If `false` returned, this is the (truthy) reason code.
   * @return `true` on success; `false` on failure.
   */
  bool make_channel_mqs(Persistent_mq_handle_from_cfg* mq_c2s,
                        Persistent_mq_handle_from_cfg* mq_s2c,
                        Shared_name* mq_name_c2s, Shared_name* mq_name_s2c,
                        Error_code* err_code);

  // Data.

  /**
   * Handles the protocol negotiation at the start of the pipe, as pertains to algorithms perpetuated by
   * the vanilla ipc::session `Session` hierarchy.
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
   * The `on_done_func` argument to async_accept_log_in(); `.empty()` except while the (at most one, ever)
   * async_accept_log_in() is outstanding.  In other words it is assigned at entry at async_accept_log_in()
   * and `.clear()`ed upon being invoked, indicating either success or failure of the log-in.  Reminder:
   * async_accept_log_in() may not be re-attempted.
   *
   * It may be invoked by dtor, if it does not fire before dtor.
   */
  flow::async::Task_asio_err m_log_in_on_done_func;

  /**
   * The pre-connected (PEER-state) socket stream peer; is non-as-if-default-cted only up to async_accept_log_in(),
   * at which point it is moved-to #m_master_channel.  Accessed only the one time, in async_accept_log_in(),
   * and immediately emptied there.  As of this writing that's in thread W, but that does not matter much.
   */
  transport::sync_io::Native_socket_stream m_master_sock_stm;

  /**
   * Unused/falsy except when in almost-PEER state (past successful async_accept_log_in(), before init_handlers()
   * invoked by user), and #m_master_channel reports incoming-direction error.  See class doc header for background.
   */
  Error_code m_pre_init_err_code;

  /// Identical to Client_session_impl::m_last_passively_opened_channel_id.
  unsigned int m_last_passively_opened_channel_id;

  /// Identical to Client_session_impl::m_last_actively_opened_channel_id.
  unsigned int m_last_actively_opened_channel_id;

  /**
   * Unique component of MQ names generated by make_channel_mqs() (if relevant; else untouched).
   * The mechanic of assigning this (1, 2, ...) is same as #m_last_passively_opened_channel_id.  Note, though,
   * this guy is very much not merely for logging: MQs must have unique names which are transmitted to the other
   * side during the channel-open procedure.
   */
  unsigned int m_last_channel_mq_id;

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
   *   - Up to async_accept_log_in(): this is null.
   *   - When async_accept_log_in() is outstanding: this is not null.
   *     - If it fails (and it cannot be re-attempted) it is renullified (though it could've been left alone too;
   *       realistically dtor is coming soon... maybe reconsider that).
   *     - Otherwise it remains non-null and immutable until dtor.
   */
  Master_structured_channel_ptr m_master_channel;

  /// See sub_class_set_deinit_func().  `.empty()` unless that was called at least once.
  Function<void ()> m_deinit_func_or_empty;

  /**
   * Null until PEER state is reached, and NULL unless compile-time #S_GRACEFUL_FINISH_REQUIRED is `true`,
   * this is used to block at the start of dtor to synchronize with the opposing `Session` dtor for safety.
   *
   * @see Session_base::Graceful_finisher doc header for all the background ever.
   */
  std::optional<typename Base::Graceful_finisher> m_graceful_finisher;
}; // class Server_session_impl

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SRV_SESSION_IMPL \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload, \
           schema::ShmType S_SHM_TYPE_OR_NONE, size_t S_SHM_MAX_HNDL_SZ, bool S_GRACEFUL_FINISH_REQUIRED_V>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SRV_SESSION_IMPL \
  Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload, \
                      S_SHM_TYPE_OR_NONE, S_SHM_MAX_HNDL_SZ, S_GRACEFUL_FINISH_REQUIRED_V>

TEMPLATE_SRV_SESSION_IMPL
CLASS_SRV_SESSION_IMPL::Server_session_impl(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                                            transport::sync_io::Native_socket_stream&& master_channel_sock_stm) :
  Base(srv_app_ref),
  flow::log::Log_context(logger_ptr, Log_component::S_SESSION),

  /* Initial protocol = 1!
   * @todo This will get quite a bit more complex, especially for m_protocol_negotiator_aux,
   *       once at least one relevant protocol gains a version 2.  See class doc header for discussion. */
  m_protocol_negotiator(get_logger(), flow::util::ostream_op_string("srv-sess-", *this), 1, 1),
  m_protocol_negotiator_aux(get_logger(), flow::util::ostream_op_string("srv-sess-aux-", *this), 1, 1),

  m_master_sock_stm(std::move(master_channel_sock_stm)),
  m_last_passively_opened_channel_id(0),
  m_last_actively_opened_channel_id(0),
  m_last_channel_mq_id(0),
  m_async_worker(get_logger(), flow::util::ostream_op_string("srv_sess[", *this, ']'))
{
  /* Accept succeeded.  We have pretty much all we need for the Channel m_master_channel.  For simplicity
   * w/r/t handling errors delay that until the async_accept_log_in() though. */

  FLOW_LOG_TRACE("Server session [" << *this << "]: Created.  The accept-log-in attempt not yet requested.  "
                 "Worker thread starting, will idle until accept-log-in.  We have the freshly-accepted "
                 "master socket stream [" << m_master_sock_stm << "].");
  m_async_worker.start();
} // Server_session_impl::Server_session_impl()

TEMPLATE_SRV_SESSION_IMPL
void CLASS_SRV_SESSION_IMPL::dtor_async_worker_stop()
{
  using flow::async::Synchronicity;
  using boost::promise;

  // We are in thread U (from either our own dtor or subclass dtor).

  assert((!m_async_worker.task_engine()->stopped()) && "This shall be called at most once.");

  /* The following is quite similar to Client_session_impl::dtor_async_worker_stop(), so keeping comments
   * light here (@todo code reuse?). */

  FLOW_LOG_INFO("Server session [" << *this << "]: Shutting down.  Worker thread will be joined, but first "
                "we perform session master channel end-sending op (flush outgoing data) until completion.  "
                "This is generally recommended at EOL of any struc::Channel, on error or otherwise.  "
                "This is technically blocking but unlikely to take long.  Also a Graceful_finisher phase may "
                "precede the above steps (it will log if so).");

  if constexpr(S_GRACEFUL_FINISH_REQUIRED)
  {
    typename Base::Graceful_finisher* graceful_finisher_or_null = {};
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
     *   - If however in the last few microseconds (or w/e) indeed m_graceful_finisher
     *     has become non-empty, then now *this is in PEER state.  ...NOPE!  The only way that can happen
     *     for us -- unlike Client_session_impl as of this writing -- is via init_handlers(), which they user
     *     themselves must execute.  So actually graceful_finisher_or_null remains accurate from up above
     *     to the next statement.  */
    if (graceful_finisher_or_null)
    {
      graceful_finisher_or_null->on_dtor_start();
      // OK, now safe (or as safe as we can guarantee) to blow *this's various parts away.
    }
  } // if constexpr(S_GRACEFUL_FINISH_REQUIRED)

  m_async_worker.post([&]()
  {
    // We are in thread W.

    if (!m_master_channel)
    {
      return; // Destroyed at early juncture (as of this writing).  Whatever; cool; nothing to flush anyway.
    }
    // else

    promise<void> done_promise;
    m_master_channel->async_end_sending([&](const Error_code&)
    {
      // We are in thread Wc (unspecified, really struc::Channel async callback thread).
      FLOW_LOG_TRACE("Server session [" << *this << "]: Shutdown: master channel outgoing-direction flush finished.");
      done_promise.set_value();
    }); // Don't care if returned false and did nothing: cool then; did our best.
    // Back here in thread W:
    done_promise.get_future().wait();
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); // m_async_worker.post()
  // Back here in thread U: done; yay.

  m_async_worker.stop();
  // Thread W has been joined.
} // Server_session_impl::dtor_async_worker_stop()

TEMPLATE_SRV_SESSION_IMPL
CLASS_SRV_SESSION_IMPL::~Server_session_impl()
{
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;

  // We are in thread U.  By contract in doc header, they must not call us from a completion handler (thread W).

  /* The following is quite similar to ~Client_session_impl(), so keeping comments light here (@todo code reuse?).
   * This is simpler after the dtor_async_worker_stop() though; async_accept_log_in() is *not* a public API;
   * so things like calling it more than 1x, let alone while an async-accept-log-in is in progress, are simply
   * not allowed and treated as undefined behavior.  Hence there's no m_mutex/m_state about which to worry. */

  if (!m_async_worker.task_engine()->stopped())
  {
    dtor_async_worker_stop();
  }

  if (!m_log_in_on_done_func.empty())
  {
    FLOW_LOG_INFO("Server session [" << *this << "]: Continuing shutdown.  Next we will run user async-accept-log-in "
                  "handler from some other thread.  In this user thread we will await its completion and then return.");

    Single_thread_task_loop one_thread(get_logger(), ostream_op_string("srv_sess[", *this, "]-temp_deinit"));
    one_thread.start([&]()
    {
      FLOW_LOG_INFO("Server session [" << *this << "]: In transient finisher thread: Shall run pending handler.");

      m_log_in_on_done_func(error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
      FLOW_LOG_TRACE("User async-accept-log-in handler finished.");
    }); // one_thread.start()
  } // if (!m_log_in_on_done_func.empty())
  // Here thread exits/joins synchronously.  Back in thread U:

  // Last thing!  See sub_class_set_deinit_func() doc header which will eventually lead you to a rationale comment.
  if (!m_deinit_func_or_empty.empty())
  {
    FLOW_LOG_INFO("Server session [" << *this << "]: Continuing shutdown.  A sub-class desires final de-init "
                  "work, once everything else is stopped (might be persistent resource cleanup); invoking "
                  "synchronously.");
    m_deinit_func_or_empty();
    FLOW_LOG_TRACE("De-init work finished.");
  }
} // Server_session_impl::~Server_session_impl()

TEMPLATE_SRV_SESSION_IMPL
const Client_app* CLASS_SRV_SESSION_IMPL::client_app() const
{
  // They are to invoke this only in almost-PEER or PEER state, at which point cli_app_ptr() is non-null + immutable.
  return Base::cli_app_ptr();
}

TEMPLATE_SRV_SESSION_IMPL
template<typename Task_err, typename On_passive_open_channel_handler>
bool CLASS_SRV_SESSION_IMPL::init_handlers(Task_err&& on_err_func_arg,
                                           On_passive_open_channel_handler&& on_passive_open_channel_func_arg)
{
  using flow::async::Task_asio_err;
  using flow::async::Synchronicity;

  // We are in thread U.

  bool result;
  m_async_worker.post([&]()
  {
    // We are in thread W.
    result = init_handlers_impl(Task_asio_err(std::move(on_err_func_arg)),
                                On_passive_open_channel_func(std::move(on_passive_open_channel_func_arg)));
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  return result;
}

TEMPLATE_SRV_SESSION_IMPL
template<typename Task_err>
bool CLASS_SRV_SESSION_IMPL::init_handlers(Task_err&& on_err_func_arg)
{
  using flow::async::Task_asio_err;
  using flow::async::Synchronicity;

  // We are in thread U.

  bool result;
  m_async_worker.post([&]()
  {
    // We are in thread W.
    result = init_handlers_impl(Task_asio_err(std::move(on_err_func_arg)),
                                On_passive_open_channel_func());
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  return result;
}

TEMPLATE_SRV_SESSION_IMPL
bool CLASS_SRV_SESSION_IMPL::init_handlers_impl
       (flow::async::Task_asio_err&& on_err_func_arg,
        On_passive_open_channel_func&& on_passive_open_channel_func_or_empty_arg)
{
  // We are in thread W.

  if (handlers_are_set())
  {
    FLOW_LOG_WARNING("Server session [" << *this << "]: User trying to proceed to PEER state by setting handlers; "
                     "but we are already in that state (handlers are set).  Ignoring.");
    return false;
  }
  // else

  FLOW_LOG_INFO("Server session [" << *this << "]: User trying to proceed to PEER state by setting handlers: "
                "Success.  Will emit any cached master-channel error ([" << m_pre_init_err_code << "] "
                "[" << m_pre_init_err_code.message() << "]).");

  Base::set_on_err_func(std::move(on_err_func_arg));
  Base::set_on_passive_open_channel_func(std::move(on_passive_open_channel_func_or_empty_arg));
  assert(handlers_are_set() && "We just set them....");

  if (m_pre_init_err_code)
  {
    assert((!Base::hosed()) && "We only just entered PEER state.");

    /* Post this asynchronously onto thread W, even though we are in W already, so that they don't delay our,
     * and therefore thread U caller's, return. */
    m_async_worker.post([this]()
    {
      // We are in thread W.

      if (Base::hosed())
      {
        FLOW_LOG_INFO("Server session [" << *this << "]: We were about to fire cached error observed earlier "
                      "during the almost-PEER phase; but somehow *yet another* error condition hosed us during "
                      "the async-gap between that recent decision and this task executing in worker thread.  "
                      "It is hard to imagine what it could be, but it is not known to be a bug or problem.  "
                      "It is interesting enough for INFO log level though.  Recommend investigation.");
      }
      else
      {
        Base::hose(m_pre_init_err_code);
      }
    });
    return true;
  }
  // else if (!m_pre_init_err_code):

  m_master_channel->expect_msgs(Master_structured_channel::Msg_which_in::OPEN_CHANNEL_TO_SERVER_REQ,
                                [this](const typename Master_structured_channel::Msg_in_ptr& open_channel_req_cref)
  {
    // We are in thread Wc (unspecified, really struc::Channel async callback thread).
    m_async_worker.post([this, open_channel_req = open_channel_req_cref]() mutable
    {
      // We are in thread W.
      on_master_channel_open_channel_req(std::move(open_channel_req));
    });
  });

  // See Session_base::Graceful_finisher doc header for all the background ever.  Also: it logs as needed.
  if constexpr(S_GRACEFUL_FINISH_REQUIRED)
  {
    m_graceful_finisher.emplace(get_logger(), this, &m_async_worker, m_master_channel.get());
  }

  return true;
} // Server_session_impl::init_handlers_impl()

TEMPLATE_SRV_SESSION_IMPL
bool CLASS_SRV_SESSION_IMPL::handlers_are_set() const
{
  return Base::on_err_func_set();
}

TEMPLATE_SRV_SESSION_IMPL
const Session_token& CLASS_SRV_SESSION_IMPL::session_token() const
{
  using flow::async::Synchronicity;

  // We are in thread U.

  const Session_token* result;
  m_async_worker.post([&]()
  {
    // We are in thread W.
    if (!handlers_are_set())
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Session-token accessor: Invoked before PEER state. "
                       "Returning null.");
      result = &transport::struc::NULL_SESSION_TOKEN;
    }
    // else

    if (Base::hosed())
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Session-token accessor: Core executing after session "
                       "hosed.  Ignoring.");
      result = &(transport::struc::NULL_SESSION_TOKEN);
      return;
    }
    // else

    assert(m_master_channel);
    result = &(m_master_channel->session_token());
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
  return *result;
} // Server_session_impl::session_token()

TEMPLATE_SRV_SESSION_IMPL
typename CLASS_SRV_SESSION_IMPL::Mdt_builder_ptr CLASS_SRV_SESSION_IMPL::mdt_builder()
{
  using util::Native_handle;
  using flow::async::Synchronicity;
  namespace asio_local = transport::asio_local_stream_socket;

  // We are in thread U.

  Channel_obj opened_channel;
  Native_handle remote_hndl_or_null;
  Shared_name mq_name_c2s_or_none;
  Shared_name mq_name_s2c_or_none;
  bool resources_acquired_ok = false;

  typename Master_structured_channel::Msg_out open_channel_req_msg;
  bool open_channel_req_msg_inited = false;
  m_async_worker.post([&]()
  {
    // We are in thread W: need to be to access m_master_channel and to call create_channel_and_resources().

    if (!handlers_are_set())
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open active request: Invoked before PEER state. "
                       "Ignoring.");
      assert((!open_channel_req_msg_inited) && "Should have remained false as initialized.");
      assert((remote_hndl_or_null.null()) && "Should have remained .null() as initialized.");
      return;
    }
    // else

    resources_acquired_ok
      = (!Base::hosed())
        && create_channel_and_resources(&mq_name_c2s_or_none, &mq_name_s2c_or_none, &remote_hndl_or_null,
                                        &opened_channel, true); // true => active-open, not passive-open.

    assert(resources_acquired_ok || remote_hndl_or_null.null());
    open_channel_req_msg = m_master_channel->create_msg(std::move(remote_hndl_or_null));
    open_channel_req_msg_inited = true;
    // (Might be sending them this Native_handle too. ---^)
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);

  // The rest we can do back in thread U again.

  if (!open_channel_req_msg_inited)
  {
    // As advertised: Previous error (reported via on-error handler) => return null.
    return Mdt_builder_ptr();
  }
  // else

  auto open_channel_root = open_channel_req_msg.body_root()->initOpenChannelToClientReq();
  if (resources_acquired_ok)
  {
    open_channel_root.setClientToServerMqAbsNameOrEmpty(mq_name_c2s_or_none.str());
    open_channel_root.setServerToClientMqAbsNameOrEmpty(mq_name_s2c_or_none.str());
  }
  /* else if (!resources_acquired_ok):
   * Resource unavailable.  Now, we know this already, even though we haven't even sent any request to the
   * client -- because we *are* the server and thus immediately can and must acquire the channel resources.
   * Our contract is to report such a problem via the later open_channel() though.  So we save this fact
   * and will report it quickly from open_channel(), right at its start.
   *
   * By saving a default-cted Channel_obj in this event, we indicate this fact.
   *
   * Rationale: Well, we promised this behavior.  But why?  We could report it now.  Answer: It would be a clumsy
   * API contract.  Either we must return `false`, in which case they cannot easily distinguish between
   * "prior incoming-direction error => false, just count on the on-error handler" and
   * "resource unavailable error => false, your decision whether to blow everything up or what.";
   * or we'd need to add an Error_code* out-arg, which means complicating the Session concept API, which means
   * changing Server_session too.  Anyway, it is weird, from user's PoV, to even deal with the idea that
   * in a Client_session the metadata-builder-returning method somehow can already fail -- they have no reason
   * to think resource acquisition occurs at that stage.  So at the expense of a short delay we just delegate
   * new-error reporting to open_channel() instead of making it an odd 2-step thing.
   *
   * That said, open_channel() contract still has to specifically note that the particular resource-unavailable
   * error -- not fatal to the Session -- is a possibility, unlike other `Error_code`s which are fatal.
   * However (1) that's open_channel()'s problem; and (2) it is manageable (and is mandated by Session concept). */

  Open_channel_req_ptr open_channel_req_ptr(new Open_channel_req
                                                  { open_channel_root.initMetadata(),
                                                    std::move(open_channel_req_msg),
                                                    // If this is empty: indicates resource-unavail eventuality.
                                                    resources_acquired_ok
                                                      ? std::move(opened_channel)
                                                      : Channel_obj() });
  return Mdt_builder_ptr(std::move(open_channel_req_ptr), &open_channel_req_ptr->m_mdt_builder);
} // Server_session_impl::mdt_builder()

TEMPLATE_SRV_SESSION_IMPL
bool CLASS_SRV_SESSION_IMPL::open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt,
                                          Error_code* err_code)
{
  using schema::detail::OpenChannelResult;
  using flow::async::Synchronicity;
  using boost::chrono::milliseconds;
  using boost::chrono::round;

  // We are in thread U.

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Server_session_impl::open_channel,
                                     target_channel, flow::util::bind_ns::cref(mdt), _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (!mdt)
  {
    FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open active request: "
                     "null metadata-builder passed-in; either user error passing in null; or more likely "
                     "that routine failed and returned null, meaning an incoming-direction error was or is being "
                     "emitted to the on-error handler.  Emitting code-less failure in this synchronous path.");
    return false;
  }
  // else

  // See mdt_builder(): It may be indicating to us the resource-unavailable error.  Check for this.

  auto& open_channel_req = *(reinterpret_cast<Open_channel_req*>(mdt.get()));
  if (!open_channel_req.m_opened_channel.initialized())
  {
    // @todo Maybe save the original Error_code responsible for this instead of emitting this catch-all thingie?
    *err_code = error::Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE;
    FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open active request: "
                     "metadata-builder indicates resources-unavailable error (not fatal to session).  Emitting error "
                     "[" << *err_code << "] [" << err_code->message() << "] without attempting any request to client.");
    return true;
  }
  // else

  // @todo The rest is way too similar to Client_session's.  Code reuse somehow?  Via Session_base perhaps?

  FLOW_LOG_INFO("Server session [" << *this << "]: Channel open active request: The blocking, but presumed "
                "sufficiently quick to be considered non-blocking, request-response exchange initiating; timeout set "
                "to a generous [" << round<milliseconds>(Base::S_OPEN_CHANNEL_TIMEOUT) << "].  The channel is already "
                "opened on our side: [" << open_channel_req.m_opened_channel << "].");

  /* As usual do all the work in thread W, where we can access m_master_channel and m_state among other things.
   * We do need to return the results of the operation here in thread U though. */
  bool sync_error_but_do_not_emit = false; // If this ends up true, err_code is ignored and clear()ed ultimately.
  // If it ends up false, *err_code will indicate either success or non-fatal error.

  m_async_worker.post([&]()
  {
    // We are in thread W.

    assert(handlers_are_set()
           && "mdt is non-null, yet state is not PEER, even though PEER is an end state?  Bug.");

    if (Base::hosed())
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open request send-request stage preempted "
                       "by earlier session-hosed error.  No-op.");
      sync_error_but_do_not_emit = true;
      return;
    }
    // else if (!Base::hosed()): We can attempt sync_request() (which could hose us itself).

    auto& open_channel_req = *(reinterpret_cast<Open_channel_req*>(mdt.get()));

    /* Now simply issue the open-channel request prepared already via `mdt` and synchronously
     * await response, or error, or timeout.  struc::Channel::sync_request() is tailor-made for this. */
    const auto open_channel_rsp = m_master_channel->sync_request(open_channel_req.m_req_msg, nullptr,
                                                                 Base::S_OPEN_CHANNEL_TIMEOUT, err_code);

    if ((!open_channel_rsp) && (!*err_code))
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open request failed at the send-request stage "
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
        FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open active request: It timed out.  It may "
                         "complete in the background still, but when it does any result shall be ignored.  "
                         "Emitting (non-session-hosing) error.");

        *err_code = error::Code::S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT;
        // Non-session hosing error will be synchronously emitted; *this will continue (as promised).
        return;
      }
      // else if (*err_code truthy but not timeout): Session got hosed.

      /* As promised, open_channel() hosing the session shall be reported asynchronously like all errors;
       * while we will merely return false. */
      FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open request failed at the sync-request stage, "
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

    FLOW_LOG_TRACE("Server session [" << *this << "]: Channel open request: Request message sent to client "
                   "sans error, and response received in time.");

    const auto root = open_channel_rsp->body_root().getOpenChannelToClientRsp();
    switch (root.getOpenChannelResult())
    {
    case OpenChannelResult::ACCEPTED:
      assert(!*err_code);
      break;
    case OpenChannelResult::REJECTED_PASSIVE_OPEN:
      *err_code = error::Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN;
      break;
    case OpenChannelResult::REJECTED_RESOURCE_UNAVAILABLE:
      assert(false && "Only server grabs channel resources; not possible in server-active-open path.");
      break;
    case OpenChannelResult::END_SENTINEL:
      assert(false);
    } // Compiler should catch missing enum value.

    if (*err_code)
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Channel open active request: Response received but "
                       "indicates remote peer (client) refused; emitting "
                       "[" << *err_code << "] [" << err_code->message() << "] with explanation from remote "
                       "peer.  This will not hose session on our end.");
      // Note: all of the above are advertised as non-session-hosing.
      return;
    }
    // else: Cool! We already have our side of the channel too.
    assert(open_channel_req.m_opened_channel.initialized());

    *target_channel = std::move(open_channel_req.m_opened_channel);

    FLOW_LOG_INFO("Server session [" << *this << "]: Channel open active request: Succeeded in time yielding "
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
} // Server_session_impl::open_channel()

TEMPLATE_SRV_SESSION_IMPL
bool CLASS_SRV_SESSION_IMPL::open_channel(Channel_obj* target_channel, Error_code* err_code)
{
  // We are in thread U.
  return open_channel(target_channel, mdt_builder(), err_code);
}

TEMPLATE_SRV_SESSION_IMPL
void CLASS_SRV_SESSION_IMPL::on_master_channel_open_channel_req
       (typename Master_structured_channel::Msg_in_ptr&& open_channel_req)
{
  using schema::detail::OpenChannelResult;
  using transport::struc::schema::Metadata;
  using util::Native_handle;
  using boost::shared_ptr;

  // We are in thread W.

  if (Base::hosed())
  {
    /* Another thread-W task hosed the session.  It shouldn't be due to m_master_channel itself firing error,
     * as then we shouldn't be called, but maybe our own logic found some session-hosing condition in the meantime. */
    return;
  }
  // else if (!Base::hosed()): Our time to shine.  We may may make Base::hosed() true ourselves, in peace, below.

  OpenChannelResult open_channel_result = OpenChannelResult::ACCEPTED;
  Channel_obj opened_channel;
  Native_handle remote_hndl_or_null;
  Shared_name mq_name_c2s_or_none;
  Shared_name mq_name_s2c_or_none;

  const auto& on_passive_open_func = Base::on_passive_open_channel_func_or_empty();
  if (on_passive_open_func.empty())
  {
    open_channel_result = OpenChannelResult::REJECTED_PASSIVE_OPEN;
  }
  else // if (passive opens enabled)
  {
    if (!create_channel_and_resources(&mq_name_c2s_or_none, &mq_name_s2c_or_none, &remote_hndl_or_null,
                                      &opened_channel, false)) // false => passive-open, not active-open.
    {
      open_channel_result = OpenChannelResult::REJECTED_RESOURCE_UNAVAILABLE;
    }
    // else if (create_channel_and_resources() worked) { Fall through. }
  } // else if (passive opens enabled)

  Mdt_reader_ptr mdt; // Null for now; set inside below in the success case.

  auto open_channel_rsp = m_master_channel->create_msg();
  const auto open_channel_req_saved = open_channel_req.get(); // It'll get move()d away below possibly; just save it.
  open_channel_rsp.body_root()->initOpenChannelToServerRsp().setOpenChannelResult(open_channel_result);

  if (open_channel_result == OpenChannelResult::ACCEPTED)
  {
    // These names may both be empty (<=> the compile-time config implies no MQs are needed).
    assert(((!remote_hndl_or_null.null()) || ((!mq_name_c2s_or_none.empty()) && (!mq_name_s2c_or_none.empty())))
           && "The channel had to have been configured to have at least one of: MQs, socket stream.");

    // remote_hndl_or_null may or may not be null (<=> the compile-time config implies no socket stream is needed).
    open_channel_rsp.store_native_handle_or_null(std::move(remote_hndl_or_null));
    assert(remote_hndl_or_null.null() && "store_native_handle_or_null() should have eaten any Native_handle.");

    auto open_channel_rsp_root = open_channel_rsp.body_root()->initOpenChannelToServerRsp();
    open_channel_rsp_root.setClientToServerMqAbsNameOrEmpty(mq_name_c2s_or_none.str());
    open_channel_rsp_root.setServerToClientMqAbsNameOrEmpty(mq_name_s2c_or_none.str());

    /* open_channel_rsp is ready to go.  That's for us to send.
     * Locally we need to emit opened_channel; that's ready too.
     * Locally, lastly, we need to emit the channel-open metadata from the open-channel request.  Take care of that
     * now.  See similar code in Client_session_impl; there's a comment explaining this in a bit more detail. */
    struct Req_and_mdt_reader
    {
      typename Mdt_reader_ptr::element_type m_mdt_reader;
      typename Master_structured_channel::Msg_in_ptr m_req;
    };
    shared_ptr<Req_and_mdt_reader> req_and_mdt(new Req_and_mdt_reader
                                                     { open_channel_req->body_root()
                                                         .getOpenChannelToServerReq().getMetadata(),
                                                       std::move(open_channel_req) });
    mdt = Mdt_reader_ptr(std::move(req_and_mdt), &req_and_mdt->m_mdt_reader);
  } // else if (open_channel_result == ACCEPTED)

  Error_code err_code;
  if (!m_master_channel->send(open_channel_rsp, open_channel_req_saved, &err_code))
  {
    return;
  }
  // else
  if (err_code)
  {
    FLOW_LOG_WARNING("Server session [" << *this << "]: Passive open-channel: send() of OpenChannelToServerRsp "
                     "failed (details above presumably); this hoses the session.  Emitting to user handler.");
    Base::hose(err_code);
    return;
  } // if (err_code on send())
  // else

  if (open_channel_result != OpenChannelResult::ACCEPTED)
  {
    // Cool.  Well, from our point of view it's cool; the user has some stuff to figure out on their app protocol level.
    FLOW_LOG_WARNING("Server session [" << *this << "]: Passive open-channel: We have rejected passive-open request "
                     "(reason explained above hopefully).  Response indicating this is sent.  Done.");
    return;
  }
  // else

  // Great!
  FLOW_LOG_INFO("Server session [" << *this << "]: Passive open-channel: Successful on this side!  Response sent; "
                "remote peer (client) shall establish its peer object once received.  We emit local peer Channel "
                "object [" << opened_channel << "], plus the channel-open metadata, "
                "to user via passive-channel-open handler.");
  on_passive_open_func(std::move(opened_channel), std::move(mdt));
  FLOW_LOG_TRACE("Handler finished.");
} // Server_session_impl::on_master_channel_open_channel_req()

TEMPLATE_SRV_SESSION_IMPL
bool CLASS_SRV_SESSION_IMPL::create_channel_and_resources(Shared_name* mq_name_c2s_or_none_ptr,
                                                          Shared_name* mq_name_s2c_or_none_ptr,
                                                          util::Native_handle* remote_hndl_or_null_ptr,
                                                          Channel_obj* opened_channel_ptr,
                                                          bool active_else_passive)
{
  using util::Native_handle;
  using transport::sync_io::Native_socket_stream;
  using transport::Socket_stream_channel;
  using transport::Mqs_channel;
  using transport::Mqs_socket_stream_channel;
  using transport::Socket_stream_channel_of_blobs;
  namespace asio_local = transport::asio_local_stream_socket::local_ns;
  using asio_local::connect_pair;
  using transport::asio_local_stream_socket::Peer_socket;
  using flow::util::ostream_op_string;

  // We are in thread W.
  auto& opened_channel = *opened_channel_ptr;
  auto& remote_hndl_or_null = *remote_hndl_or_null_ptr;
  auto& mq_name_c2s_or_none = *mq_name_c2s_or_none_ptr;
  auto& mq_name_s2c_or_none = *mq_name_s2c_or_none_ptr;

  assert(!opened_channel.initialized());
  assert(remote_hndl_or_null.null());
  assert(mq_name_c2s_or_none.empty());
  assert(mq_name_s2c_or_none.empty());

  Native_socket_stream local_sock_stm_or_null;
  Error_code sys_err_code;
  const auto nickname = active_else_passive ? ostream_op_string("active", ++m_last_actively_opened_channel_id)
                                            : ostream_op_string("passive", ++m_last_passively_opened_channel_id);

  [[maybe_unused]] const auto make_sock_stm_func = [&]()
  {
    Native_handle local_hndl;

    Peer_socket local_hndl_asio(*m_async_worker.task_engine());
    Peer_socket remote_hndl_asio(std::move(local_hndl_asio));
    connect_pair(local_hndl_asio, remote_hndl_asio, sys_err_code);
    if (sys_err_code)
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Open-channel (active? = [" << active_else_passive << "]): "
                       "connect_pair() failed.  Not fatal to the session; will inform the remote peer (client) "
                       "(if passive-open) or caller (if active-open).  Details follow.");
      FLOW_ERROR_SYS_ERROR_LOG_WARNING(); // Log based on sys_err_code.
      return false;
    }
    // else
    local_hndl = Native_handle(local_hndl_asio.release());
    remote_hndl_or_null = Native_handle(remote_hndl_asio.release());

    FLOW_LOG_TRACE("Server session [" << *this << "]: Open-channel (active? = [" << active_else_passive << "]): "
                   "connect_pair() generated native handles [" << local_hndl << "], [" << remote_hndl_or_null << "], "
                   "the latter to be transmitted to remote peer (client).");
    local_sock_stm_or_null = Native_socket_stream(get_logger(), nickname, std::move(local_hndl));
    return true;
  }; // make_sock_stm_func =

  if constexpr(S_MQS_ENABLED)
  {
    Persistent_mq_handle_from_cfg mq_c2s;
    Persistent_mq_handle_from_cfg mq_s2c;

    if (!make_channel_mqs(&mq_c2s, &mq_s2c, &mq_name_c2s_or_none, &mq_name_s2c_or_none, &sys_err_code))
    {
      return false;
    }
    // else

    assert((!sys_err_code) && "It should have returned false on truthy Error_code.");
    if constexpr(S_SOCKET_STREAM_ENABLED)
    {
      static_assert(std::is_same_v<Channel_obj,
                                   Mqs_socket_stream_channel<true, Persistent_mq_handle_from_cfg>>, "Sanity check.");
      if (make_sock_stm_func())
      {
        opened_channel = Channel_obj(get_logger(), nickname, std::move(mq_s2c), std::move(mq_c2s),
                                     std::move(local_sock_stm_or_null), &sys_err_code);
      }
      else
      {
        assert(sys_err_code); // Handled a bit lower down.
      }
      // Fall through.
    }
    else // if constexpr(!S_SOCKET_STREAM_ENABLED)
    {
      static_assert(std::is_same_v<Channel_obj,
                                   Mqs_channel<true, Persistent_mq_handle_from_cfg>>, "Sanity check.");
      opened_channel = Channel_obj(get_logger(), nickname, std::move(mq_s2c), std::move(mq_c2s), &sys_err_code);
    }

    if (sys_err_code)
    {
      // Clean up the stuff that *was* created OK.  Use tactic from inside error path in make_channel_mqs().
      mq_c2s = Persistent_mq_handle_from_cfg();
      mq_s2c = Persistent_mq_handle_from_cfg();
      Error_code ignored;
      Persistent_mq_handle_from_cfg::remove_persistent(get_logger(), mq_name_c2s_or_none, &ignored);
      Persistent_mq_handle_from_cfg::remove_persistent(get_logger(), mq_name_s2c_or_none, &ignored);

      opened_channel = Channel_obj(); // For cleanliness / no pointless threads sitting around....
      return false;
    }
    // else { Yay! }
  } // if constexpr(S_MQS_ENABLED)
  else // if constexpr(!S_MQS_ENABLED)
  {
    static_assert(S_SOCKET_STREAM_ENABLED, "Either MQs or socket stream or both must be configured on somehow.");

    if (!make_sock_stm_func())
    {
      return false;
    }
    // else

    if constexpr(S_TRANSMIT_NATIVE_HANDLES)
    {
      static_assert(std::is_same_v<Channel_obj, Socket_stream_channel<true>>, "Sanity check.");
      opened_channel = Channel_obj(get_logger(), nickname, std::move(local_sock_stm_or_null)); // Yay!
    }
    else
    {
      static_assert(std::is_same_v<Channel_obj, Socket_stream_channel_of_blobs<true>>, "Sanity check.");
      opened_channel = Channel_obj(get_logger(), nickname, std::move(local_sock_stm_or_null)); // Yay!
    }
  } // else if constexpr(!S_MQS_ENABLED)

  return true;
} // Server_session_impl::create_channel_and_resources()

TEMPLATE_SRV_SESSION_IMPL
bool CLASS_SRV_SESSION_IMPL::make_channel_mqs(Persistent_mq_handle_from_cfg* mq_c2s,
                                              Persistent_mq_handle_from_cfg* mq_s2c,
                                              Shared_name* mq_name_c2s, Shared_name* mq_name_s2c,
                                              Error_code* err_code)
{
  using std::to_string;

  assert(mq_c2s && mq_s2c);
  assert(err_code);

  /* By default in Linux POSIX MQs this happens to be the actual limit for # of unread messages --
   * visible in /proc/sys/fs/mqueue/msg_max -- so we cannot go higher typically.  However that file can be modified.
   * For now let's assume a typical environment.
   * @todo Persistent_mq_handle_from_cfg might be actually bipc MQ, with no such limit, though.  So we're (1)
   * assuming a certain default and (2) taking POSIX MQs as the lowest common denominator (metaphorically speaking).
   * So really this could be something else in that case.  Reconsider / make configurable / etc.  Might also use the
   * default /proc/sys/fs/mqueue/msg_default which is possible using a certain mq_ API.  Likely such niceties would
   * be factored out into Persistent_mq_handle concept API as opposed to litigated here.
   *
   * To be clear: the transport:: MQ stuff above the lowest (Persistent_mq_handle) level will seamlessly handle
   * any would-block situation due to reaching this limit.  In theory it could affect perf. */
  constexpr size_t MAX_N_MSG = 10;

  /* We must also decide the max message of each size.  If SHM-backing is disabled, then there's no question:
   * we have to use Base::S_MQS_MAX_MSG_SZ which (see its doc header) respects both the need to be as large
   * as possible to support the widest variety of user payloads; and the (POSIX-MQ-relevant) system limit(s) as
   * to the message size.  If SHM-backing is enabled, it is tempting to simply use the same value... why not,
   * right?  Good enough for the heap-backed case; why not the SHM-backed case?  At worst it only wastes some
   * RAM, since each message transmitted over MQ contains merely a tiny SHM handle, right?  Yes and no:
   * For bipc MQs, yes; but POSIX MQs impose yet another limit: A given process trying to maintain a *total*
   * of X bytes of MQs at a given time (the "total" roughly meaning the sum of `MAX_N_MSG x max_msg_sz` for
   * all then-opened MQs) will yield too-many-files (Linux errno=EMFILES) once X is exceeded.  A typical value
   * for this is 819,200 -- or up to only *ten* MAX_N_MSxMQS_MAX_MSG_SZ such MQs opened simultaneously.
   * This is in practice easily exceeded.  On the other hand, suppose we instead use S_SHM_MAX_HNDL_SZ
   * instead of MQS_MAX_MSG_SZ -- which is sufficient, since we'll only be sending SHM handles, not the actual
   * data.  As of this writing it's 64; so that's 8,192/64=128 *times* the number of MQs creatable this way;
   * errno=EMFILES occurs not after 10 MQs now but after 1,280 such MQs.  Much more reasonable.
   * Trust me (ygoldfel): the ~10-MQ limit can be quite annoying. */
  size_t max_msg_sz;
  if constexpr(S_SHM_ENABLED)
  {
    max_msg_sz = S_SHM_MAX_HNDL_SZ;
  }
  else
  {
    max_msg_sz = Base::S_MQS_MAX_MSG_SZ;
  }

  ++m_last_channel_mq_id;

  const auto mq_name_func = [&](bool c2s) -> Shared_name
  {
    // Session scope.
    auto mq_name = build_conventional_shared_name(Persistent_mq_handle_from_cfg::S_RESOURCE_TYPE_ID,
                                                  Shared_name::ct(Base::m_srv_app_ref.m_name),
                                                  Base::srv_namespace(),
                                                  Shared_name::ct(Base::cli_app_ptr()->m_name),
                                                  Base::cli_namespace());
    // That was the standard stuff.  Now the us-specific stuff:
    mq_name /= c2s ? "c2s" : "s2c";
    mq_name /= to_string(m_last_channel_mq_id);
    return mq_name;
  }; // mq_name_func =

  // These ctors should log in any case.

  const auto perms = util::shared_resource_permissions(Base::m_srv_app_ref.m_permissions_level_for_client_apps);

  *mq_c2s = Persistent_mq_handle_from_cfg(get_logger(), mq_name_func(true), util::CREATE_ONLY,
                                          MAX_N_MSG, max_msg_sz, perms, err_code);
  if (*err_code)
  {
    --m_last_channel_mq_id; // Might as well.
    return false;
  }
  // else
  *mq_s2c = Persistent_mq_handle_from_cfg(get_logger(), mq_name_func(false), util::CREATE_ONLY,
                                          MAX_N_MSG, max_msg_sz, perms, err_code);
  if (*err_code)
  {
    --m_last_channel_mq_id; // Might as well.
    const auto name = mq_c2s->absolute_name();
    *mq_c2s = Persistent_mq_handle_from_cfg(); // Close the first guy; won't be used.
    Error_code ignored; // Don't forget to remove underlying MQ we'd successfully created.
    Persistent_mq_handle_from_cfg::remove_persistent(get_logger(), name, &ignored);
    return false;
  }
  // else
  *mq_name_c2s = mq_c2s->absolute_name();
  *mq_name_s2c = mq_s2c->absolute_name();
  return true;
} // Server_session_impl::make_channel_mqs()

TEMPLATE_SRV_SESSION_IMPL
template<typename Session_server_impl_t,
         typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_SRV_SESSION_IMPL::async_accept_log_in
       (Session_server_impl_t*,
        // (^-- Unused here; but it's a customization point across multiple class templates.)
        Channels* init_channels_by_srv_req, Mdt_reader_ptr* mdt_from_cli_or_null, Channels* init_channels_by_cli_req,
        Cli_app_lookup_func&& cli_app_lookup_func, Cli_namespace_func&& cli_namespace_func,
        Pre_rsp_setup_func&& pre_rsp_setup_func,
        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func, Mdt_load_func&& mdt_load_func,
        Task_err&& on_done_func)
{
  using util::Process_credentials;
  using flow::async::Task_asio_err;
  using flow::util::ostream_op_string;
  using boost::make_shared;
  using boost::shared_ptr;
  using std::string;

  // We are in thread U.

  // Reminder: We are protected -- not public -- and can only be called *once*.

  m_log_in_on_done_func = std::move(on_done_func); // May be invoked from thread W or thread U (dtor) (it's a race).

  // Kick off all work from thread W to avoid concurrency.
  m_async_worker.post([this,
                       init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                       cli_app_lookup_func = std::move(cli_app_lookup_func),
                       cli_namespace_func = std::move(cli_namespace_func),
                       pre_rsp_setup_func = std::move(pre_rsp_setup_func),
                       n_init_channels_by_srv_req_func = std::move(n_init_channels_by_srv_req_func),
                       mdt_load_func = std::move(mdt_load_func)]
                        () mutable
  {
    // We are in thread W.

    /* We'll want to check these OS-reported remote-process credentials against something during the log-in
     * procedure that is coming next; per remote_peer_process_credentials() doc header it's best to call it
     * first-thing (before traffic if possible) so let's. */
    Error_code err_code;
    string os_proc_invoked_as;
    const auto os_proc_creds = m_master_sock_stm.remote_peer_process_credentials(&err_code);
    if (err_code)
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in request: We have the freshly-accepted "
                       "master socket stream [" << m_master_sock_stm << "]; but "
                       "then obtaining OS-reported peer process credentials immediately failed: "
                       "[" << err_code << "] [" << err_code.message() << "].  Shall close stream and execute handler.");
    }
    else if (os_proc_invoked_as = os_proc_creds.process_invoked_as(&err_code), err_code) // Check err_code result....
    {
      FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in request: We have the freshly-accepted "
                       "master socket stream [" << m_master_sock_stm << "]; and "
                       "then obtaining OS-reported peer process credentials immediately succeded yielding "
                       "[" << os_proc_creds << "], but trying to query the executable-invoked-as (binary name) "
                       "value failed ([" << err_code << "] [" << err_code.message() << "]) (e.g., is it still "
                       "running?).  Shall close stream and execute handler.");
    }
    // else { err_code is still falsy. }

    if (err_code)
    {
      // Might as well do it now instead of waiting for dtor (kind of consistent with m_master_channel.reset() below).
      m_master_sock_stm = transport::sync_io::Native_socket_stream();

      m_log_in_on_done_func(err_code);
      FLOW_LOG_TRACE("Handler finished.");
      m_log_in_on_done_func.clear();
      return;
    }
    // else

    FLOW_LOG_INFO("Server session [" << *this << "]: Accept-log-in request: We have the freshly-accepted "
                  "master socket stream [" << m_master_sock_stm << "]; "
                  "wrapping in 1-pipe Channel; then wrapping that in a direct-heap-serializing struc::Channel; "
                  "then waiting until log-in request arrives; and only then we will send response, at which point "
                  "we will issue the user handler indicating the session is ready.");

    transport::Socket_stream_channel<true>
      unstructured_master_channel(get_logger(), ostream_op_string("smc-", *this),
                                  std::move(m_master_sock_stm));
    {
      const auto opposing_proc_creds = unstructured_master_channel.remote_peer_process_credentials(&err_code);
      assert((!err_code) && "It really should not fail that early.  If it does look into this code.");
      FLOW_LOG_INFO("Server session [" << *this << "]: Opposing process info: [" << opposing_proc_creds << "].");
    }
    // ...sock_stm is now hosed.  We have the Channel.

    /* Finally: eat the Channel; make it live inside m_master_channel.  This session master channel requires
     * a log-in phase (the only such channel in the session), so use that ctor. */
    assert(!m_master_channel);
    m_master_channel = make_shared<Master_structured_channel>(get_logger(), std::move(unstructured_master_channel),
                                                              transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP,
                                                              true); // We are the server, not the client.
    // unstructured_master_channel is now hosed.

    /* Enable liveness checking and keep-alive; the other side will do the same.
     * Per auto_ping() and idle_timer_run() concept docs, one uses *just* the latter only if the protocol is such
     * that our own application-layer messages are known to be sent at least at certain intervals.  Our protocol
     * is not that way, so per those docs use auto_ping() to periodically declare liveness.  Thus we use this
     * mechanism (in both directions) to detect unspecified zombie/death situations of the unstructured transport
     * layer.  E.g., if the other side is too dead to send pings or anything else, we will know and hopefully
     * shut the session down before things go really bad. */
    m_master_channel->owned_channel_mutable()->auto_ping();
    m_master_channel->owned_channel_mutable()->idle_timer_run();

    m_master_channel->start([this](const Error_code& channel_err_code)
    {
      // We are in thread Wc (unspecified, really struc::Channel async callback thread).
      m_async_worker.post([this, channel_err_code]() { on_master_channel_error(channel_err_code); });
    });

    /* Let's overview our task as the session-opening server.  We do the other side of what Client_session_impl does
     * in it async_connect() flow, but we have one fewer phase: we already have the connected Native_socket_stream
     * and even have subsumed it in a Channel and even have subsumed *that* in a struc::Channel already.
     * So we just need to do the server side of the log-in.  It may help to follow along that part of the code
     * in Client_session.
     *
     * OK!  m_master_channel is rocking... but in the log-in phase (as server).  Follow the quite-rigid steps
     * documented for that phase namely: expect the log-in request message and respond with log-in response
     * message (first checking validity of various stuff in the former).  So this is the
     * async step (we must await the response).  It should be quite quick, as the client is doing its half of
     * the algorithm right now, but I digress. */
    const bool result = m_master_channel->expect_log_in_request
                          (Master_structured_channel::Msg_which_in::LOG_IN_REQ,
                           [this, os_proc_creds, os_proc_invoked_as = std::move(os_proc_invoked_as),
                            init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                            cli_app_lookup_func = std::move(cli_app_lookup_func),
                            cli_namespace_func = std::move(cli_namespace_func),
                            pre_rsp_setup_func = std::move(pre_rsp_setup_func),
                            n_init_channels_by_srv_req_func = std::move(n_init_channels_by_srv_req_func),
                            mdt_load_func = std::move(mdt_load_func)]
                             (typename Master_structured_channel::Msg_in_ptr&& log_in_req) mutable
    {
      // We are in thread Wc (unspecified, really struc::Channel async callback thread).
      m_async_worker.post([this, os_proc_creds, os_proc_invoked_as = std::move(os_proc_invoked_as),
                           log_in_req = std::move(log_in_req),
                           init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                           cli_app_lookup_func = std::move(cli_app_lookup_func),
                           cli_namespace_func = std::move(cli_namespace_func),
                           pre_rsp_setup_func = std::move(pre_rsp_setup_func),
                           n_init_channels_by_srv_req_func = std::move(n_init_channels_by_srv_req_func),
                           mdt_load_func = std::move(mdt_load_func)]()
      {
        // We are in thread W.

        assert(m_master_channel
               && "We should not get invoked, if on_master_channel_error() was invoked, and "
                    "the latter should be the only way to nuke m_master_channel at this stage.");

        Error_code err_code; // A few things can go wrong below; start with success assumption.

        const auto msg_root = log_in_req->body_root().getLogInReq();
        // (Initialize to avoid -- incorrect -- warning.)
        size_t n_init_channels = 0; // If successful log-in (so far) set this to how many init channels we'll open.
        size_t n_init_channels_by_cli_req = 0;
        Mdt_reader_ptr mdt_from_cli; // If successful log-in (so far) set this to eventual *mdt_from_cli_or_null result.

        /* Please see "Protocol negotiation" in Client_session_impl class doc header for discussion; and notes in the
         * schema .capnp file around ProtocolNegotiation-typed fields.
         *
         * Protocol-negotiation occurs *here*, before any further fields are interpreted. */
        const auto proto_neg_root = msg_root.getProtocolNegotiationToServer();
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

            if ((msg_root.getShmTypeOrNone() != S_SHM_TYPE)
                || (msg_root.getMqTypeOrNone() != S_MQ_TYPE_OR_NONE)
                || (msg_root.getNativeHandleTransmissionEnabled() != S_TRANSMIT_NATIVE_HANDLES))
            {
              FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: Log-in request received; "
                               "but client desired process SHM/channel config (SHM-type "
                               "[" << int(msg_root.getShmTypeOrNone()) << "], MQ-type "
                               "[" << int(msg_root.getMqTypeOrNone()) << "], transmit-native-handles? = "
                               "[" << msg_root.getNativeHandleTransmissionEnabled() << "]) "
                               "does not match our local config ([" << int(S_SHM_TYPE) << '/'
                               << int(S_MQ_TYPE_OR_NONE) << '/' << S_TRANSMIT_NATIVE_HANDLES << "]).  "
                               "Perhaps the other side used Client_session variant class not matching local "
                               "Server_session variant class and/or different template parameters.  "
                               "Closing session master channel and reporting to user via on-accept-log-in handler.");
              err_code = error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH;
            } // if (msg_root.shmType, mqType, ... are not OK)
            else // if (msg_root.shmType, mqType, ... are OK)
            {
              n_init_channels_by_cli_req = msg_root.getNumInitChannelsByCliReq();
              if ((n_init_channels_by_cli_req != 0) && (!init_channels_by_cli_req))
              {
                FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: Log-in request received; "
                                 "but this side passed null container for "
                                 "init-channels-by-cli-request, while client indicates it will open "
                                 "[" << n_init_channels_by_cli_req << "] of them -- this or other side misbehaved?  "
                                 "Closing session master channel and reporting to user via on-accept-log-in handler.");
                err_code = error::Code::S_INVALID_ARGUMENT;
              }
              else // if (n_init_channels_by_cli_req is OK)
              {
                /* @todo We are assuming good-faith here about the message's contents; should probably be more paranoid
                 * for consistency regarding, like, a null claimed_own_proc_creds.  Change assert() to a real error
                 * maybe, etc. */
                assert(msg_root.hasClaimedOwnProcessCredentials());

                auto claimed_proc_creds_root = msg_root.getClaimedOwnProcessCredentials();
                const Process_credentials claimed_proc_creds(claimed_proc_creds_root.getProcessId(),
                                                             claimed_proc_creds_root.getUserId(),
                                                             claimed_proc_creds_root.getGroupId());

                const string cli_app_name(msg_root.getOwnApp().getName());
                const Client_app* const cli_app_ptr_or_null = cli_app_lookup_func(cli_app_name);

                if ((!cli_app_ptr_or_null) ||
                    (!flow::util::key_exists(Base::m_srv_app_ref.m_allowed_client_apps, cli_app_name)))
                {
                  FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: Log-in request received (claimed "
                                   "client process creds [" << claimed_proc_creds << "], "
                                   "OS-reported client process creds [" << os_proc_creds << "], "
                                   "OS-reported invoked-as name [" << os_proc_invoked_as << "], "
                                   "cli-app-name [" << cli_app_name << "]); but the latter is unknown or not in the "
                                   "allow-list for the present server-app [" << Base::m_srv_app_ref << "].  "
                                   "Closing session master channel and reporting to user via on-accept-log-in "
                                   "handler.");
                  err_code = error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN;
                }
                else // if (cli_app_ptr_or_null)
                {
                  Base::set_cli_app_ptr(cli_app_ptr_or_null);

                  /* Nice, the other side's Client_app is valid and allowed.  Run safety checks on claimed_proc_creds;
                   * they must reconfirm the Client_app they mean is the Client_app we know by checking for consistency
                   * of the values against our Client_app config; plus ensure consistency by checking gainst the
                   * OS-reported values (in case it's some kind snafu or spoofing or who knows). */
                  if ((claimed_proc_creds != os_proc_creds)
                      || (Base::cli_app_ptr()->m_exec_path.string() != os_proc_invoked_as)
                      || (Base::cli_app_ptr()->m_user_id != claimed_proc_creds.user_id())
                      || (Base::cli_app_ptr()->m_group_id != claimed_proc_creds.group_id()))
                  {
                    FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: Log-in request received "
                                     "(claimed client process creds [" << claimed_proc_creds << "], "
                                     "OS-reported client process creds [" << os_proc_creds << "], "
                                     "OS-reported invoked-as name [" << os_proc_invoked_as << "], "
                                     "client-app [" << *(Base::cli_app_ptr()) << "]); and the latter is known and in "
                                     "allow-list for the present server-app [" << Base::m_srv_app_ref << "].  "
                                     "However the aforementioned process creds (UID:GID) do not match "
                                     "the aforementioned locally-configured client-app values UID:GID; and/or "
                                     "the claimed creds (UID:GID, PID) do not match the OS-reported one from stream; "
                                     "and/or OS-reported invoked-as name does not *exactly* match client-app exec "
                                     "path.  For safety: Closing session master channel and reporting to user via "
                                     "on-accept-log-in handler.");
                    err_code = error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS;
                  }
                  else // if (creds are consistent)
                  {
                    // Even nicer, Client_app is totally correct.  So we can do this to ~complete Session_base stuff.
                    Base::set_cli_namespace(cli_namespace_func());

                    // As promised run the setup thing just before sending the success response to opposing client.

                    assert(!err_code);
                    if ((err_code = pre_rsp_setup_func()))
                    {
                      FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: Log-in request received "
                                       "(claimed client process creds [" << claimed_proc_creds << "], "
                                       "OS-reported client process creds [" << os_proc_creds << "], "
                                       "OS-reported invoked-as name [" << os_proc_invoked_as << "], "
                                       "client-app [" << *(Base::cli_app_ptr()) << "]); and the latter is known and in "
                                       "the allow-list for the present server-app [" << Base::m_srv_app_ref << "].  "
                                       "Everything matches.  However the per-app setup code yielded error "
                                       "(details above).  Giving up; will not send log-in response.  "
                                       "Closing session master channel and reporting to user via "
                                       "on-accept-log-in handler.");
                    }
                    else // if (!(err_code = pre_rsp_setup_func()))
                    {
                      /* Cool then.  We can send the log-in response.  Now is a great time to 1, fish out the
                       * cli->srv metadata (to pass set out-arg to before successful user-handler execution);
                       * 2, ask local user (via in-arg function) how many channels it wants opened (they need
                       * various cli->srv info to make this decision); 3, compute srv->cli metadata
                       * (they need... ditto).  This stuff cannot fail. */

                      // Firstly: the cli->srv metadata.
                      const auto log_in_req_saved = log_in_req.get(); // It'll get move()d away below; save it.
                      {
                        /* Only set *mdt_from_cli_or_null when invoking handler on success as promised; untouched
                         * otherwise as promised.  Use the same technique as when emitting `mdt` in
                         * on_master_channel_open_channel_req(); omitting comments. */
                        struct Req_and_mdt_reader
                        {
                          typename Mdt_reader_ptr::element_type m_mdt_reader;
                          typename Master_structured_channel::Msg_in_ptr m_req;
                        };
                        shared_ptr<Req_and_mdt_reader> req_and_mdt(new Req_and_mdt_reader
                                                                         { msg_root.getMetadata(),
                                                                           std::move(log_in_req) });
                        // (log_in_req is now hosed.)
                        mdt_from_cli = Mdt_reader_ptr(std::move(req_and_mdt), &req_and_mdt->m_mdt_reader);
                      } // {}

                      // Secondly: n_init_channels (we have n_init_channels_by_cli_req; now add the local count).
                      n_init_channels = n_init_channels_by_cli_req;
                      if (init_channels_by_srv_req)
                      {
                        n_init_channels += n_init_channels_by_srv_req_func(*(Base::cli_app_ptr()),
                                                                           n_init_channels_by_cli_req,
                                                                           Mdt_reader_ptr(mdt_from_cli));
                      }

                      // Thirdly: srv->cli metadata.  This goes inside LogInRsp; so create that now.
                      auto log_in_rsp_msg = m_master_channel->create_msg();
                      auto log_in_rsp_root = log_in_rsp_msg.body_root()->initLogInRsp();
                      auto mdt_from_srv_root = log_in_rsp_root.initMetadata();
                      // And let them fill it out.
                      mdt_load_func(*(Base::cli_app_ptr()), n_init_channels_by_cli_req, Mdt_reader_ptr(mdt_from_cli),
                                    &mdt_from_srv_root); // They load this (if they want).

                      // Now we can send the log-in response finally.

                      // Similarly to Client_session_impl: give them our version info, so they can verify, as we did.
                      auto proto_neg_root = log_in_rsp_root.initProtocolNegotiationToClient();
                      proto_neg_root.setMaxProtoVer(m_protocol_negotiator.local_max_proto_ver_for_sending());
                      proto_neg_root.setMaxProtoVerAux(m_protocol_negotiator_aux.local_max_proto_ver_for_sending());

                      log_in_rsp_root.setClientNamespace(Base::cli_namespace().str());
                      log_in_rsp_root.setNumInitChannelsBySrvReq(n_init_channels - n_init_channels_by_cli_req);

                      assert(!err_code);
                      if (!m_master_channel->send(log_in_rsp_msg, log_in_req_saved, &err_code))
                      {
                        /* The docs say send() returns false if:
                         * "`msg` with-handle, but #Owner_channel has no handles pipe; an #Error_code was previously
                         * emitted via on-error handler; async_end_sending() has already been called."  Only the middle
                         * one is possible in our case.  on_master_channel_error() shall issue
                         * m_log_in_on_done_func(truthy) in our stead. */
                        FLOW_LOG_TRACE("send() synchronously failed; must be an error situation.  Error handler should "
                                       "catch it shortly or has already caught it.");
                        return;
                      }
                      // else either it succeeded (err_code still falsy); or it failed (truthy now).

                      if (err_code)
                      {
                        FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: Log-in request received "
                                         "(claimed client process creds [" << claimed_proc_creds << "], "
                                         "OS-reported client process creds [" << os_proc_creds << "], "
                                         "OS-reported invoked-as name [" << os_proc_invoked_as << "], "
                                         "client-app [" << *(Base::cli_app_ptr()) << "]); and the latter is known and "
                                         "in allow-list for the present server-app [" << Base::m_srv_app_ref << "].  "
                                         "Init-channel count expected is [" << n_init_channels << "]; of these "
                                         "[" << n_init_channels_by_cli_req << "] requested by opposing client.  "
                                         "Everything matches.  "
                                         "Log-in response (including saved cli-namespace "
                                         "[" << Base::cli_namespace() << "] and filled-out srv->cli metadata) "
                                         "send attempted, but the send failed (details above).  "
                                         "Closing session master channel and reporting to user via "
                                         "on-accept-log-in handler.");
                      }
                      else // if (!err_code)
                      {
                        // Excellent.  m_master_channel auto-proceeds to logged-in phase on successful send().
                        FLOW_LOG_INFO("Server session [" << *this << "]: Accept-log-in: Log-in request received "
                                      "(claimed client process creds [" << claimed_proc_creds << "], "
                                      "OS-reported client process creds [" << os_proc_creds << "], "
                                      "OS-reported invoked-as name [" << os_proc_invoked_as << "], "
                                      "client-app [" << *(Base::cli_app_ptr()) << "]); and the latter is known and in "
                                      "allow-list for the present server-app [" << Base::m_srv_app_ref << "].  "
                                      "Everything matches.  "
                                      "Log-in response, including saved cli-namespace "
                                      "[" << Base ::cli_namespace() << "], sent OK.  "
                                      "Init-channel count expected is [" << n_init_channels << "]; of these "
                                      "[" << n_init_channels_by_cli_req << "] requested by opposing client.  "
                                      "If 0: Will go to almost-PEER state and report to user via on-accept-log-in "
                                      "handler (user must then invoke init_handlers() on the Server_session to reach "
                                      "PEER state).  "
                                      "Else: Will send that many open-channel-to-client requests with the init-channel "
                                      "info; and await one ack in return.");
                      } // else if (!err_code from send())
                    } // else if (!(err_code = pre_rsp_setup_func())) (err_code might have become truthy inside though)
                  } // else if (creds are consistent) (ditto re. err_code)
                } // else if (cli_app_ptr_or_null) (ditto re. err_code)
              } // else if (n_init_channels_by_cli_req is OK) (ditto re. err_code)
            } // else if (msg_root.getShmTypeOrNone() == S_SHM_TYPE_OR_NONE) (ditto re. err_code)
          } // else if (!err_code from m_protocol_negotiator_aux.compute_negotiated_proto_ver()) (ditto re. err_code)
        } // else if (!err_code from m_protocol_negotiator.compute_negotiated_proto_ver()) (ditto re. err_code)

        // Whew.  OK then.  Did we pass the gauntlet?

        if (err_code)
        {
          // As promised in the various error paths above: error => end the short-lived master channel.
          m_master_channel.reset();
        }
        // As noted in the message just-logged (whether error or not):
        if (err_code || (n_init_channels == 0))
        {
          if ((!err_code) && mdt_from_cli_or_null)
          {
            // This out-arg still applies; n_init_channels being 0 is orthogonal!
            *mdt_from_cli_or_null = std::move(mdt_from_cli);
          }

          m_log_in_on_done_func(err_code);
          FLOW_LOG_TRACE("Handler finished.");
          m_log_in_on_done_func.clear();
          return;
        }
        // else if ((!err_code) && (n_init_channels != 0)):

        Channels init_channels; // Put all init-channels here first.  Out-args touched only on ultimate success.
        init_channels.reserve(n_init_channels);
        bool ok = true;
        for (size_t idx = 0; (idx != n_init_channels) && ok; ++idx)
        {
          init_channels.resize(idx + 1);
          auto& opened_channel = init_channels.back();
          util::Native_handle remote_hndl_or_null;
          Shared_name mq_name_c2s_or_none;
          Shared_name mq_name_s2c_or_none;

          if (!create_channel_and_resources(&mq_name_c2s_or_none, &mq_name_s2c_or_none, &remote_hndl_or_null,
                                            &opened_channel, true)) // true => active-open, not passive-open.
          {
            err_code = error::Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE;
            ok = false;
            continue; // Abort.
          }
          // else
          auto open_channel_req_msg = m_master_channel->create_msg(std::move(remote_hndl_or_null));
          auto open_channel_root = open_channel_req_msg.body_root()->initOpenChannelToClientReq();
          open_channel_root.setClientToServerMqAbsNameOrEmpty(mq_name_c2s_or_none.str());
          open_channel_root.setServerToClientMqAbsNameOrEmpty(mq_name_s2c_or_none.str());

          /* Expect quick reply to the last one (we don't even check contents; it's just an ack, so
           * `ok` = <it returned non-null>).  @todo Consider using timeout sync_request() overload just in case. */
          ok = (idx == (n_init_channels - 1))
                 ? bool(m_master_channel->sync_request(open_channel_req_msg, nullptr, &err_code))
                 : (m_master_channel->send(open_channel_req_msg, nullptr, &err_code)
                      && (!err_code));
        } // for (idx in n_init_channels)

        if (ok)
        {
          // Excellent.  We can go to almost-PEER state/emit everything.
          FLOW_LOG_INFO("Server session [" << *this << "]: Accept-log-in: Init-channels opened/client acked.  "
                        "Will go to almost-PEER state and report to user via on-accept-log-in "
                        "handler (user must then invoke init_handlers() on the Server_session to reach PEER "
                        "state).");
          size_t idx = 0;
          if (init_channels_by_cli_req)
          {
            auto& init_channels_ref = *init_channels_by_cli_req;
            init_channels_ref.clear();
            init_channels_ref.reserve(n_init_channels_by_cli_req);
            for (; idx != n_init_channels_by_cli_req; ++idx)
            {
              init_channels_ref.emplace_back(std::move(init_channels[idx]));
              FLOW_LOG_INFO("Server session [" << *this << "]: Emitting init-channel (cli-requested): "
                            "[" << init_channels_ref.back() << "].");
            }
          }
          if (init_channels_by_srv_req)
          {
            auto& init_channels_ref = *init_channels_by_srv_req;
            init_channels_ref.clear();
            init_channels_ref.reserve(n_init_channels - idx);
            for (; idx != n_init_channels; ++idx)
            {
              init_channels_ref.emplace_back(std::move(init_channels[idx]));
              FLOW_LOG_INFO("Server session [" << *this << "]: Emitting init-channel (srv-requested): "
                            "[" << init_channels_ref.back() << "].");
            }
          }
          assert(idx == n_init_channels);
          // OK: Channel out-args all the way from async_connect() have been set.  Now same for mdt; invoke handler.

          if (mdt_from_cli_or_null)
          {
            *mdt_from_cli_or_null = std::move(mdt_from_cli);
          }

          assert(!err_code);
        } // if (ok)
        else // if (!ok)
        {
          /* Something (create_channel_and_resources() or send()) failed; loop aborted.
           * We won't emit any of init_channels, so it'll all get cleaned up fine.  So abort session-open as usual. */
          if (!err_code)
          {
            FLOW_LOG_TRACE("send() synchronously failed; must be an error situation.  Error handler should "
                           "catch it shortly or has already caught it.");
            return;
          }
          /* else if (err_code) // I.e., we detected new error ourselves.  sync_request()'s TIMEOUT is not
           *                    // channel-hosing, by the way, but to us it's fatal anyway. */

          // Again: As promised in the various error paths above: error => end the short-lived master channel.
          m_master_channel.reset();

          FLOW_LOG_WARNING("Server session [" << *this << "]: Accept-log-in: A send() of an init-channel's info "
                           "failed (details logged above).  "
                           "Closing session master channel and reporting to user via "
                           "on-accept-log-in handler.");
        } // else if (!ok)

        // err_code may me truthy or falsy.
        m_log_in_on_done_func(err_code);
        FLOW_LOG_TRACE("Handler finished.");
        m_log_in_on_done_func.clear();
      }); // m_async_worker.post()
    }); // const bool result = m_master_channel->expect_log_in_request()

    if (!result)
    {
      /* The docs say expect_log_in_request() returns false if:
       * "expect_log_in_request() has already been invoked, if log-in phase is not active or active in the client
       * role, or if a prior error has hosed the owned Channel."  Only the latter is possible in our case.
       * on_master_channel_error() shall issue m_log_in_on_done_func(truthy) in our stead. */
      FLOW_LOG_TRACE("expect_log_in_request() synchronously failed; must be an error situation.  Error handler should "
                     "catch it shortly or has already caught it.");
    }
  }); // m_async_worker.post()
} // Server_session_impl::async_accept_log_in()

TEMPLATE_SRV_SESSION_IMPL
void CLASS_SRV_SESSION_IMPL::on_master_channel_error(const Error_code& err_code)
{
  // We are in thread W.

  /* (If you are comparing this to Client_session::on_master_channel_error(), you might notice this is a lot simpler
   * (at least in the log-in phase); that is because Client_session supports multiple async_connect()s in series
   * (if 1 or more fail), so m_master_channel can be nullified and then again constructed.  In our case
   * async_accept_log_in() is a protected API, made public via Server_session_dtl to Session_server
   * only, and therefore (due to having a limited internal use case) can be only called 1x, by contract.
   * Hence there is no chance of getting an err_code that pertains to some past m_master_channel and all that
   * stuff; so we don't need that observer weak_ptr stuff.  It's a much simpler state machine (there's no m_state
   * for instance).) */

  if (!m_master_channel)
  {
    FLOW_LOG_TRACE("Server session [" << *this << "]: Session master channel reported error; but the source "
                   "master channel has since been destroyed; so presumably the log-in request was received "
                   "from remote client, and it was invalid, so the channel was destroyed by our log-in request "
                   "handler; but coincidentally before that destruction could occur an incoming-direction "
                   "channel error occurred.  The error does not matter anymore.  Ignoring.");
    return;
  }
  // else

  // Might want to read our class doc header impl section regarding error handling.  Then come back here.

  if (!m_log_in_on_done_func.empty())
  {
    // We are in logging-in phase still.
    FLOW_LOG_WARNING("Server session [" << *this << "]: Log-in: Session master channel reported error "
                     "[" << err_code << "] [" << err_code.message() << "].  Will go back to NULL "
                     "state and report to user via on-accept-log-in handler.");

    /* Channel incoming-direction error while awaiting log-in request.  Might as well clean this up; though
     * it would be cleaned up presumably pretty soon, as Session_server will destroy *this. */
    m_master_channel.reset();

    m_log_in_on_done_func(err_code);
    FLOW_LOG_TRACE("Handler finished.");
    m_log_in_on_done_func.clear();
    return;
  } // if (!m_log_in_on_done_func.empty()): We are in logged-in phase: almost-PEER state or PEER state.

  if (!handlers_are_set())
  {
    FLOW_LOG_WARNING("Server session [" << *this << "]: Almost-PEER state: Session master channel reported error "
                     "[" << err_code << "] [" << err_code.message() << "].  Recording it to emit once "
                     "user advances us to PEER state by setting handlers.");

    assert((!m_pre_init_err_code) && "struc::Channel should emit no more than one fatal error.");
    m_pre_init_err_code = err_code;
    return;
  }
  // else if (handlers_are_set()): We are in PEER state.  Here the logic is similar to Client_session_impl again:

  if (!Base::hosed())
  {
    Base::hose(err_code);
  }

  if constexpr(S_GRACEFUL_FINISH_REQUIRED)
  {
    m_graceful_finisher->on_master_channel_hosed();
  }
} // Server_session_impl::on_master_channel_error()

TEMPLATE_SRV_SESSION_IMPL
const typename CLASS_SRV_SESSION_IMPL::Base& CLASS_SRV_SESSION_IMPL::base() const
{
  return *(static_cast<const Base*>(this));
}

TEMPLATE_SRV_SESSION_IMPL
template<typename Task>
void CLASS_SRV_SESSION_IMPL::sub_class_set_deinit_func(Task&& task)
{
  m_deinit_func_or_empty = std::move(task);
}

TEMPLATE_SRV_SESSION_IMPL
flow::async::Single_thread_task_loop* CLASS_SRV_SESSION_IMPL::async_worker()
{
  return &m_async_worker;
}

TEMPLATE_SRV_SESSION_IMPL
typename CLASS_SRV_SESSION_IMPL::Master_structured_channel* CLASS_SRV_SESSION_IMPL::master_channel()
{
  return m_master_channel.get();
}

TEMPLATE_SRV_SESSION_IMPL
const typename CLASS_SRV_SESSION_IMPL::Master_structured_channel& CLASS_SRV_SESSION_IMPL::master_channel_const() const
{
  return *m_master_channel;
}

TEMPLATE_SRV_SESSION_IMPL
std::ostream& operator<<(std::ostream& os, const CLASS_SRV_SESSION_IMPL& val)
{
  using std::string;

  /* Let's print only really key stuff here; various internal code likes to ostream<< *this session (`val` to us),
   * and we don't want to be treating stuff here as immutable or safe to access, even though it might change
   * concurrently.  So we intentionally just print the Client_app and the Server_app (which are themselves immutable,
   * but the ref/ptr to each inside `val` might not be).  Now, for Server_session, m_srv_app_ref is immutable
   * (it's a ref!).  cli_app_ptr(), though, might change in one spot.  For that reason Session_base declared it
   * as atomic<>.  So the following is safe. */

  const auto cli_app_ptr = val.cli_app_ptr(); // Very rarely this could change underneath us (atomic<>ally).
  os << '[' << val.m_srv_app_ref.m_name << "<-" << (cli_app_ptr ? cli_app_ptr->m_name : string("unknown-cli"));
  if constexpr(CLASS_SRV_SESSION_IMPL::S_SHM_ENABLED)
  {
    os << " | shm_type=" << int(CLASS_SRV_SESSION_IMPL::S_SHM_TYPE);
  }
  return os << "]@" << static_cast<const void*>(&val);
}

#undef CLASS_SRV_SESSION_IMPL
#undef TEMPLATE_SRV_SESSION_IMPL

} // namespace ipc::session
