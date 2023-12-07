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
#include "ipc/session/detail/session_shared_name.hpp"
#include "ipc/session/schema/detail/session_master_channel.capnp.h"
#include "ipc/session/app.hpp"
#include "ipc/transport/struc/channel.hpp"
#include "ipc/transport/native_socket_stream_acceptor.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"

namespace ipc::session
{

// Types.

/**
 * Internal type containing data and types common to internal types Server_session_impl and Client_session_impl
 * which are the respective true cores of #Server_session and #Client_session respectively.
 *
 * A #Server_session and Client_session_impl share many basic properties, some public.  For example consider
 * that a Server_session_impl on this side has 1 counterpart Client_session_impl on the other side (or vice versa); for
 * both it is salient which Server_app is on the server side and which Client_app is on the client side.
 * In terms of types/constants, on each side the two Session objects must be identically configured via various
 * template params which results in non-trivial type aliases like the highly significant #Channel_obj,
 * exposed in the Session concept, and constants like #S_SOCKET_STREAM_ENABLED.
 *
 * Regarding data: an operational (PEER-state) Session (on either end) will need various members to be set
 * before it is indeed in PEER state.  This object stores such data, usually unset at first, and features
 * `protected` setters to invoke once each, until all are set permanently for PEER state.
 *
 * @tparam Mdt_payload
 *         See #Server_session, #Client_session (or Session concept).
 * @tparam S_MQ_TYPE_OR_NONE
 *         See #Server_session, #Client_session.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         See #Server_session, #Client_session.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Session_base :
  private boost::noncopyable
{
public:
  // Constants.

  /// See Session_mv.
  static constexpr bool S_MQS_ENABLED = S_MQ_TYPE_OR_NONE != schema::MqType::NONE;

  /// See Session_mv.
  static constexpr bool S_SOCKET_STREAM_ENABLED = (!S_MQS_ENABLED) || S_TRANSMIT_NATIVE_HANDLES;

  // Types.

  static_assert(S_MQ_TYPE_OR_NONE != schema::MqType::END_SENTINEL,
                "Do not use the value END_SENTINEL for S_MQ_TYPE_OR_NONE; it is only a sentinel.  Did you mean NONE?");

  // Ensure the definitions immediately following are based on correct assumptions.
  static_assert(std::is_enum_v<schema::MqType>,
                "Sanity-checking capnp-generated MqType enum (must be an enum).");
  static_assert(std::is_unsigned_v<std::underlying_type_t<schema::MqType>>,
                "Sanity-checking capnp-generated MqType enum (backing type must be unsigned).");
  static_assert((int(schema::MqType::END_SENTINEL) - 1) == 2,
                "Code apparently must be updated -- expected exactly 2 MqType enum values plus NONE + sentinel.");
  static_assert((int(schema::MqType::POSIX) != int(schema::MqType::BIPC))
                 && (int(schema::MqType::POSIX) > int(schema::MqType::NONE))
                 && (int(schema::MqType::BIPC) > int(schema::MqType::NONE))
                 && (int(schema::MqType::POSIX) < int(schema::MqType::END_SENTINEL))
                 && (int(schema::MqType::BIPC) < int(schema::MqType::END_SENTINEL)),
                "Code apparently must be updated -- "
                  "expected exactly 2 particular MqType enum values plus NONE + sentinel.");

  /**
   * Relevant only if #S_MQS_ENABLED, this is the Persistent_mq_handle-concept impl type specified by
   * the user via `S_MQ_TYPE_OR_NONE`.
   */
  using Persistent_mq_handle_from_cfg
    = std::conditional_t<!S_MQS_ENABLED,
                         transport::Null_peer,
                         std::conditional_t<S_MQ_TYPE_OR_NONE == schema::MqType::POSIX,
                                            transport::Posix_mq_handle,
                                            transport::Bipc_mq_handle>>;

  /// See Session_mv (or Session concept).
  using Channel_obj = std::conditional_t<S_MQS_ENABLED,
                                         std::conditional_t
                                           <S_TRANSMIT_NATIVE_HANDLES,
                                            transport::Mqs_socket_stream_channel<true, Persistent_mq_handle_from_cfg>,
                                            transport::Mqs_channel<true, Persistent_mq_handle_from_cfg>>,
                                         std::conditional_t
                                           <S_TRANSMIT_NATIVE_HANDLES,
                                            transport::Socket_stream_channel<true>,
                                            transport::Socket_stream_channel_of_blobs<true>>>;

  /// See Session_mv.  Note: If changed from `vector` please update those doc headers too.
  using Channels = std::vector<Channel_obj>;

  /// See Session_mv (or Session concept).
  using Mdt_payload_obj = Mdt_payload;

  /// See Session_mv (or Session concept).
  using Mdt_reader_ptr
    = boost::shared_ptr<typename transport::struc::schema::Metadata<Mdt_payload_obj>::Reader>;

  /// See Session_mv (or Session concept).
  using Mdt_builder
    = typename transport::struc::schema::Metadata<Mdt_payload_obj>::Builder;

  /// See Session_mv (or Session concept).
  using Mdt_builder_ptr = boost::shared_ptr<Mdt_builder>;

  /// See Session_mv (or Session concept).
  using Structured_msg_builder_config = transport::struc::Heap_fixed_builder::Config;

  /// See Session_mv (or Session concept).
  using Structured_msg_reader_config = transport::struc::Heap_reader::Config;

  // Methods.

  /**
   * See Server_session_impl, Client_session_impl.  However in Session_base this value may be not-yet-set (empty), or
   * set (and immutable from then on).
   *
   * This value shall be known and immutable from construction for Server_session, since the server namespace -- PID
   * as of this writing -- can be determined from the start on the server side and applies to every server session
   * that Session_server produces.  Client_session, however, determines
   * it at the latest possible moment which is at Client_session::async_connect() time, at which point it needs
   * to determine the PID via PID file.
   *
   * @return See above.
   */
  const Shared_name& srv_namespace() const;

  /**
   * See Server_session_impl, Client_session_impl.  However in Session_base this value may be not-yet-set (empty), or
   * set (and immutable from then on).
   *
   * This value shall be generated uniquely (within some context) for each new Server_session produced by
   * Session_server; and Client_session shall find that out while logging in (last part of entering PEER
   * state).
   *
   * @return See above.
   */
  const Shared_name& cli_namespace() const;

  /**
   * See Server_session_impl, Client_session_impl.  However in Session_base this value may be not-yet-set (null), or set
   * (and immutable from then on).  Note the value to which we refer is the actual pointer.  (The Client_app that
   * is pointed-to, itself, is certainly immutable too.)
   *
   * This value shall be set from the start in a Client_session but determined during a given Server_session's log-in
   * (the opposing Client_session will transmit the Client_app::m_name).  The log-in shall complete the Server_session's
   * entry to PEER state.
   *
   * @return See above.
   */
  const Client_app* cli_app_ptr() const;

  /**
   * Computes the name of the interprocess named-mutex used to control reading/writing to the file
   * storing (written by server, read by client) the value for srv_namespace().  The file
   * is located as cur_ns_store_absolute_path().
   *
   * This may be called anytime.
   *
   * @return See above.
   */
  Shared_name cur_ns_store_mutex_absolute_name() const;

  /**
   * Computes the absolute path to file storing (written by server, read by client) the value for srv_namespace().
   * The file is located as cur_ns_store_absolute_path().
   *
   * This may be called anytime.
   *
   * @return See above.
   */
  fs::path cur_ns_store_absolute_path() const;

  /**
   * Computes the absolute name at which the server shall set up a transport::Native_socket_stream_acceptor
   * to which client shall transport::Native_socket_stream::async_connect() in order to establish a PEER-state
   * session.
   *
   * This must be called no earlier than set_srv_namespace(); otherwise behavior undefined (assertion
   * may trip).
   *
   * @return See above.
   */
  Shared_name session_master_socket_stream_acceptor_absolute_name() const;

  /**
   * See Session_mv::heap_fixed_builder_config() (1-arg).
   *
   * @param logger_ptr
   *        See above.
   * @return See above.
   */
  static Structured_msg_builder_config heap_fixed_builder_config(flow::log::Logger* logger_ptr);

  /**
   * See Session_mv::heap_reader_config() (1-arg).
   *
   * @param logger_ptr
   *        See above.
   * @return See above.
   */
  static Structured_msg_reader_config heap_reader_config(flow::log::Logger* logger_ptr);

  // Data.

  /**
   * Reference to Server_app (referring to local process in Server_session, opposing process in Client_session).
   * This is known from construction and immutable (both the reference, of course, and the Server_app itself).
   */
  const Server_app& m_srv_app_ref;

protected:
  // Constants.

  /**
   * Internal timeout for `open_channel()`.
   *
   * ### The value ###
   * We initially tried something much less generous, 500ms.  It worked fine, but some people encountered
   * the timeout due to unrelated reasons, and it was natural to blame it on the timeout.  This much longer
   * timeout should make it obvious, in such a situation, that it's not about some slowness inside Flow-IPC
   * but a pathological application problem -- particularly around session-open time.  For example not
   * calling Server_session::init_handlers(), or calling it late, as of this writing can cause issues with this.
   *
   * The downside is it makes Session::open_channel() potentially blocking formally speaking, whereas 500ms
   * could still claim to be non-blocking.  It's a matter of perspective really.  This value just seems to
   * cause less confusion.  We might reconsider the whole thing however.
   */
  static constexpr util::Fine_duration S_OPEN_CHANNEL_TIMEOUT = boost::chrono::seconds(60);

  /**
   * The max sendable MQ message size as decided by Server_session_impl::make_channel_mqs() (and imposed on both sides,
   * both directions), if #S_MQS_ENABLED *and* Server_session_impl::S_SHM_ENABLED is `false`, when a channel is opened
   * (regardless of which side did the active-open or requested pre-opening at session start).
   * If `*this` belongs to Server_session_impl, that's what this is.
   * If it belongs to Client_session_impl, then this is what the opposing process -- if they're using the same code! --
   * will have decided.
   *
   * Our own heap_fixed_builder_config(), forwarded to Session_mv::heap_fixed_builder_config(), similarly uses
   * this constant in a matching way.
   *
   * @note If Server_session_impl::S_SHM_ENABLED is `true`, then a different (much smaller) MQ message size limit
   *       is configured.  In that case, also, heap_fixed_builder_config() is not relevant and should not be used.
   *
   * While it looks simple, there is a number of subtleties one must understand if *ever* considering changing it.
   *
   * ### bipc versus POSIX MQs ###
   * As of this writing the same constant value is used for both types of MQ configurable.  The actual value is
   * chosen due to a certain aspect of POSIX MQs (discussed just below); and we reuse it for bipc, though we absolutely
   * do not have to, for simplicity/for lack of better ideas at the moment.  It would be possible to bifurcate those
   * two cases if really desired.
   *
   * ### Why 8Ki? ###
   * By default in Linux POSIX MQs this happens to be the actual limit for # of unread messages --
   * visible in /proc/sys/fs/mqueue/msgsize_max -- so we cannot go higher typically.  However that file can be modified.
   * For now we assume a typical environment; or at least that it will not go *below* this typical default.
   * If did try a higher number here, opening of MQs by server will likely emit an error and refuse
   * (Server_session_impl::make_channel_mqs()).
   *
   * ### Things to consider if changing the value away from the above ###
   * - Contemplate why you're doing it.  bipc MQs are seen (in Boost source) to be a simple zero-copy data structure
   *   in an internally maintained kernel-persistent SHM pool; while I (ygoldfel) haven't verified via kernal source,
   *   likely Linux POSIX MQ impl is something very similar (reasoning omitted but trust me).  So copy perf should
   *   not be a factor; only RAM use and the functional ability to transmit messages of a certain size.
   *   - If the plan is to use `transport::struc::Heap_fixed_builder`-backed for structured messages
   *     (via transport::struc::Channel), then the max size is quite important: if a *leaf* in your message exceeds
   *     this size when serialized, it is a fatal error.
   *   - If, however, the plan is to use SHM-backing (e.g., via `shm::classic::*_session`), then this constant
   *     does not get used (a much smaller value does -- so small it would be ~meaningless to decrease it).
   * - Suppose you have changed this value, and suppose the `Heap_fixed_builder`-based use case *does* matter.
   *   If ::ipc has not yet been released in production, ever, then it's fine (assuming, that is, it'll work
   *   in the first place given the aforementioned `/proc/sys/...` limit for POSIX MQs).  If it *has* been
   *   released then there is an annoying subtlety to consider:
   *   - If you can guarantee (via release process) that the client and server will always use the same ::ipc software,
   *     then you're still fine.  Just change this value; done.  Otherwise though:
   *   - There's the unfortunate caveat that is Session_base::heap_fixed_builder_config().  There you will note
   *     it says that the *server* decides (for both sides) what this value is.  In that case that method
   *     will be correct in the server process; but if the client process is speaking to a different version
   *     of the server, with a different value for #S_MQS_MAX_MSG_SZ, then that is a potential bug.
   *     - Therefore it would be advisable to not mess with it (again... once a production version is out there).
   *       If you *do* mess with it, there are ways to ensure it all works out anyway: logic could be added
   *       wherein the client specifies its own #S_MQS_MAX_MSG_SZ when issuing an open-channel request,
   *       and the server must honor it in its Server_session_impl::make_channel_mqs().  So it can be done --
   *       just know that in that case you'll have to actually add such logic; or somewhat break
   *       heap_fixed_builder_config().  The reason I (ygoldfel) have not done this already is it seems unlikely
   *       (for various reasons listed above) that tweaking this value is of much practical value.
   */
  static constexpr size_t S_MQS_MAX_MSG_SZ = 8 * 1024;

  // Types.

  /**
   * The (internally used) session master channel is a transport::struc::Channel of this concrete type.
   *
   * Rationale for the chosen knob values:
   *   - To be able to open_channel() (and hence passive-open) a #Channel_obj that can transmit native handles,
   *     certainly a handles pipe is required to transmit half of each socket-pair.  That said, an `Mqs_channel`
   *     would work fine if #S_SOCKET_STREAM_ENABLED is `false`.  The real reason at least a handles pipe is
   *     required is that to establish this channel -- unlike subsequent channels in the session -- a client-server
   *     flow is required.  This connection establishment is not possible with MQs, so of the available options
   *     only a socket stream would work.
   *     - There is no bulky data transfers over this channel, so adding
   *       a parallel MQ-based blobs pipe is overkill even if it could help performance; more so since the
   *       master channel is unlikely to be used frequently (open_channel() on either side shouldn't be that frequent).
   *       Since, again, the minute differences in perf versus a Unix-domain-socket-based transport are unlikely to
   *       be significant in this use case, it is easier to use a socket stream which lacks any kernel-persistent
   *       cleanup considerations -- or even just distinct naming considerations for that matter.
   *     - So: use a transport::Socket_stream_channel.
   *   - Use the specially tailored (internal) session master channel capnp schema, whose key messages cover (at least):
   *     - session log-in at the start;
   *     - open-channel request/response.
   *   - As of this writing, for serializing/deserializing, it's either the heap-allocating engine on either side,
   *     or it's something SHM-based (which could be faster).  However at least some SHM-based builders/readers
   *     themselves (internally) require a transport::Channel to function, which would require a Session typically,
   *     creating a cyclical feature dependency.  Plus, SHM has cleanup considerations.  So a heap-based engine
   *     on either side is the natural choice.  The cost is some perf: one copies from heap into the session master
   *     transport; and from there into heap on the other side.  In this context that's not a significant perf loss.
   */
  using Master_structured_channel
    = transport::struc::Channel_via_heap<transport::Socket_stream_channel<true>,
                                         schema::detail::SessionMasterChannelMessageBody
                                           <Mdt_payload_obj>>;

  /**
   * Handle to #Master_structured_channel.
   *
   * ### Rationale for type chosen ###
   * It's a handle at all, because at first and possibly later there may be no session master channel, so a null
   * value is useful.  (Though, `std::optional` could be used instead.)  It's a ref-counted pointer as opposed
   * to `unique_ptr` so that it can be observed via #Master_structured_channel_observer (`weak_ptr`) which is not
   * (and cannot) be available for `unique_ptr`.  The observing is needed tactically for certain async lambda needs.
   */
  using Master_structured_channel_ptr = boost::shared_ptr<Master_structured_channel>;

  /// Observer of #Master_structured_channel_ptr.  See its doc header.
  using Master_structured_channel_observer = boost::weak_ptr<Master_structured_channel>;

  /// Concrete function type for the on-passive-open handler (if any), used for storage.
  using On_passive_open_channel_func = Function<void (Channel_obj&& new_channel,
                                                      Mdt_reader_ptr&& new_channel_mdt)>;

  // Constructors.

  /**
   * Constructs: Client_session form (the user is the one constructing the object, though in NULL state).
   * The values taken as args are set permanently (undefined behavior/assertion may trip if an attempt is made to
   * modify one via mutator).  The remaining values must be set via mutator before PEER state.
   *
   * @param cli_app_ref
   *        See cli_app_ptr().
   * @param srv_app_ref
   *        See #m_srv_app_ref.
   * @param on_err_func
   *        On-error handler from user.
   * @param on_passive_open_channel_func_or_empty_arg
   *        On-passive-open handler from user (empty if user wishes the disable passive-opens on this side).
   */
  explicit Session_base(const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                        flow::async::Task_asio_err&& on_err_func,
                        On_passive_open_channel_func&& on_passive_open_channel_func_or_empty_arg);

  /**
   * Constructs: Server_session form (Session_server is the one constructing the object, though in NULL state,
   * before log-in has completed, but after the socket-stream connection has been established).
   * The values taken as args are set permanently (undefined behavior/assertion may trip if an attempt is made to
   * modify one via mutator).  The remaining values must be set via mutator before PEER state.
   *
   * @param srv_app_ref
   *        See #m_srv_app_ref.
   */
  explicit Session_base(const Server_app& srv_app_ref);

  // Methods.

  /**
   * Sets srv_namespace() (do not call if already set).
   * @param srv_namespace_new
   *        Value.
   */
  void set_srv_namespace(Shared_name&& srv_namespace_new);

  /**
   * Sets cli_namespace() (do not call if already set).
   * @param cli_namespace_new
   *        Value.
   */
  void set_cli_namespace(Shared_name&& cli_namespace_new);

  /**
   * Sets cli_app_ptr() (do not call if already set).
   * @param cli_app_ptr_new
   *        Value.
   */
  void set_cli_app_ptr(const Client_app* cli_app_ptr_new);

  /**
   * Sets on_passive_open_channel_func_or_empty() (do not call if already set; do not call if user intends for
   * passive-opens to be disabled on this side).
   *
   * @param on_passive_open_channel_func
   *        Value.
   */
  void set_on_passive_open_channel_func(On_passive_open_channel_func&& on_passive_open_channel_func);

  /**
   * Sets on_err_func() (do not call if already set).
   *
   * @param on_err_func_arg
   *        Value.
   */
  void set_on_err_func(flow::async::Task_asio_err&& on_err_func_arg);

  /**
   * Returns `true` if and only if set_on_err_func() has been called, or .
   * @return See above.
   */
  bool on_err_func_set() const;

  /**
   * The on-passive-open handler (may be empty even in final state, meaning user wants passive-opens disabled
   * on this side).
   * @return See above.
   */
  const On_passive_open_channel_func& on_passive_open_channel_func_or_empty() const;

  /**
   * Marks this session as hosed for (truthy) reason `err_code`; and *synchronously* invokes on-error handler;
   * only invoke if not already hosed().
   *
   * This utility is important and has certain pre-conditions (behavior undefined if not met; assertion may trip):
   *   - `*this` must be in PEER state.  In particular on_err_func_set() must return `true`.
   *   - hosed() must return `false`.
   *   - `err_code` must be truthy (non-success).
   *   - This must be invoked from a thread such that it is OK to *synchronously* invoke on-error handler.
   *     As of this writing this is Server_session's or Client_session's thread W in practice, which is how
   *     thread safety of hose() versus hosed() is also guaranteed by those users.
   *
   * @param err_code
   *        Truthy error.
   */
  void hose(const Error_code& err_code);

  /**
   * Returns `true` if and only if hose() has been called.  If so then #m_on_err_func has been executed already.
   * `*this` must be in PEER state.  In particular on_err_func_set() must return `true`.
   *
   * @return Ditto.
   */
  bool hosed() const;

private:
  // Data.

  /**
   * See cli_app_ptr().
   *
   * ### Rationale for it being `atomic<>` ###
   * It is for the following specific reason.  Consider `ostream<<` of `*this`.  `*this` is really either
   * in Client_session or Server_session.  In both cases these `ostream<<`s access only #m_srv_app_ref, which
   * is immutable throughout (so no problem there) and #m_cli_app_ptr.  For Client_session #m_cli_app_ptr is
   * also immutable throughout (so no problem there).  For Server_session however it does change in one spot:
   * Server_session::async_accept_log_in() internal async handler for the log-in request shall, on success,
   * call set_cli_app_ptr() and change it from null to non-null.  This could cause concurrent access to the
   * data member, even though it's a mere pointer (but we don't count on the alleged "atomicity" of this; it is
   * generally considered not safe).
   *
   * Now, as of this writing, there's exactly one spot where `ostream<<` could be invoked from thread U while
   * it is being assigned in thread W: `async_accept_log_in()` is called by our internal code in
   * Session_server and only once; the only thing that can occur in thread U until the log-in response
   * handler is executed is that Server_session_impl dtor is called.  Before that dtor stops that thread W,
   * it does print the Server_session_impl once as of this writing.  Therefore, out of sheer caution, this guy
   * is `atomic<>`.  That said there could be other such invocations, as code might change during maintenance
   * in the future, in which case this `atomic<>`ness will quietly come in handy.
   */
  std::atomic<const Client_app*> m_cli_app_ptr;

  /// See srv_namespace().
  Shared_name m_srv_namespace;

  /// See cli_namespace().
  Shared_name m_cli_namespace;

  /// See set_on_err_func().
  flow::async::Task_asio_err m_on_err_func;

  /// See on_passive_open_channel_func_or_empty().
  On_passive_open_channel_func m_on_passive_open_channel_func_or_empty;

  /**
   * Starts falsy; becomes forever truthy (with a specific #Error_code that will not change thereafter)
   * once hose() is called (with that truthy value).  Note hose() may not be called before PEER state, which implies
   * #m_on_err_func is non-empty.
   *
   * ### Concurrency ###
   * See hose() and hosed() doc headers.  TL;DR: It is up to the caller to only call those, basically, from
   * thread W only.
   */
  Error_code m_peer_state_err_code_or_ok;
}; // class Session_base

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SESSION_BASE \
  template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SESSION_BASE \
  Session_base<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>

// Template implementations.

TEMPLATE_SESSION_BASE
CLASS_SESSION_BASE::Session_base(const Client_app& cli_app_ref, const Server_app& srv_app_ref,
                                 flow::async::Task_asio_err&& on_err_func,
                                 On_passive_open_channel_func&& on_passive_open_channel_func_or_empty_arg) :
  /* This is, basically, the (protected) ctor for Client_session, followed by Client_session::async_connect().
   * By the time we're constructed, by definition Client_app and, therefore-ish, m_on_*_func are known.
   * However: m_srv_namespace is looked-up (in file system, as of this writing) at the forthcoming async_connect();
   * empty for now; set_srv_namespace() invoked at that time.   m_cli_namespace is returned by server in
   * log-in response; set_cli_namespace() invoked at that time. */

  m_srv_app_ref(srv_app_ref), // Not copied!
  m_cli_app_ptr(&cli_app_ref), // Ditto!
  m_on_err_func(std::move(on_err_func)),
  m_on_passive_open_channel_func_or_empty(std::move(on_passive_open_channel_func_or_empty_arg))
  // m_srv_namespace and m_cli_namespace remain .empty() for now.
{
  // Yep.
}

TEMPLATE_SESSION_BASE
CLASS_SESSION_BASE::Session_base(const Server_app& srv_app_ref) :
  /* This is, basically, the (protected) ctor for Server_session, followed by
   * Server_session_impl::async_accept_log_in(), itself `protected` as of this writing.  Various items are not
   * known until that async-succeeds; Client_app is not known (until it is identified in the log-in exchange), and,
   * therefore-ish, m_on_*_func are also unknown.
   * Server_app is still known from the very start.  Moreover, due to the way m_srv_namespace is determined (our own
   * PID), we can confidently initialize that from the start (ourselves being the server).
   * m_cli_namespace is generated on server side during log-in proceedings; set_cli_namespace() invoked at that time. */

  m_srv_app_ref(srv_app_ref), // Not copied!
  m_cli_app_ptr(0), // null for now.
  // As promised, we know our own srv-namespace:
  m_srv_namespace(Shared_name::ct(std::to_string(util::Process_credentials::own_process_id())))
  // m_cli_namespace + m_on_err_func and m_on_passive_open_channel_func_or_empty remain .empty() for now.
{
  // Yep.
}

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::set_cli_app_ptr(const Client_app* cli_app_ptr_new)
{
  assert(cli_app_ptr_new && "As of this writing cli_app_ptr should be set at most once, from null to non-null.");

#ifndef NDEBUG
  const auto prev = m_cli_app_ptr.exchange(cli_app_ptr_new);
  assert((!prev) && "As of this writing cli_app_ptr should be set at most once, from null to non-null.");
#else
  m_cli_app_ptr = cli_app_ptr_new;
#endif
}

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::set_srv_namespace(Shared_name&& srv_namespace_new)
{
  assert(!srv_namespace_new.empty());
  assert((srv_namespace_new != Shared_name::S_SENTINEL) && "That is a reserved sentinel value.");
  assert(m_srv_namespace.empty() && "As of this writing srv_namespace should be set at most once, from empty.");

  m_srv_namespace = std::move(srv_namespace_new);
}

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::set_cli_namespace(Shared_name&& cli_namespace_new)
{
  assert(!cli_namespace_new.empty());
  assert((cli_namespace_new != Shared_name::S_SENTINEL) && "That is a reserved sentinel value.");
  assert(m_cli_namespace.empty() && "As of this writing cli_namespace should be set at most once, from empty.");

  m_cli_namespace = std::move(cli_namespace_new);
}

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::set_on_passive_open_channel_func(On_passive_open_channel_func&& on_passive_open_channel_func)
{
  assert(m_on_passive_open_channel_func_or_empty.empty());
  m_on_passive_open_channel_func_or_empty = std::move(on_passive_open_channel_func);
}

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::set_on_err_func(flow::async::Task_asio_err&& on_err_func_arg)
{
  assert(!on_err_func_arg.empty());
  assert(m_on_err_func.empty() && "Call set_on_err_func() once at most and only if not already set through ctor.");

  m_on_err_func = std::move(on_err_func_arg);
}

TEMPLATE_SESSION_BASE
const Client_app* CLASS_SESSION_BASE::cli_app_ptr() const
{
  return m_cli_app_ptr.load(); // Fetch atomic<> payload and return it.  (Could write `return m_cli_app_ptr;` too.)
}

TEMPLATE_SESSION_BASE
const Shared_name& CLASS_SESSION_BASE::srv_namespace() const
{
  return m_srv_namespace;
}

TEMPLATE_SESSION_BASE
const Shared_name& CLASS_SESSION_BASE::cli_namespace() const
{
  return m_cli_namespace;
}

TEMPLATE_SESSION_BASE
const typename CLASS_SESSION_BASE::On_passive_open_channel_func&
  CLASS_SESSION_BASE::on_passive_open_channel_func_or_empty() const
{
  return m_on_passive_open_channel_func_or_empty;
}

TEMPLATE_SESSION_BASE
bool CLASS_SESSION_BASE::on_err_func_set() const
{
  return !m_on_err_func.empty();
}

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::hose(const Error_code& err_code)
{
  assert((!hosed()) && "By contract do not call unless hosed() is false.");

  m_peer_state_err_code_or_ok = err_code;
  m_on_err_func(err_code);
} // Session_base::hose()

TEMPLATE_SESSION_BASE
bool CLASS_SESSION_BASE::hosed() const
{
  assert(on_err_func_set() && "By contract do not call until PEER state -- when on-error handler must be known.");
  return bool(m_peer_state_err_code_or_ok);
}

TEMPLATE_SESSION_BASE
Shared_name CLASS_SESSION_BASE::cur_ns_store_mutex_absolute_name() const
{
  using std::to_string;

  // Global scope.  Pertains to Server_app::m_name, *all* processes thereof, *all* Client_apps, *all* sessions.
  auto mutex_name = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_MUTEX,
                                                   Shared_name::ct(m_srv_app_ref.m_name),
                                                   Shared_name::S_SENTINEL);
  /* That was the standard stuff.  Now the us-specific stuff:
   *
   * Above we specified it's a mutex; now for what's this mutex?  It's for the CNS (Current Namespace Store),
   * a/k/a the PID file. */
  mutex_name /= "cur_ns_store";

  /* Further differentiate the names by owner/permissions that will be used when creating the file.
   * This is in response to seeing, during testing, the situation where (in that particular case)
   * m_permissions_level_for_client_apps changed from an accidental S_NO_ACCESS to S_(something sane).
   * The file was created with 0000 permissions, then later no one could open it (or create it, since it existed)
   * due to it being untouchable, essentially forever without manual intervention (such as `rm`ing it).
   * The following, which should be otherwise harmless, should at least help avoid such conflicts.
   * (Is it harmless?  Almost, yes: more files can potentially be created and sit around until reboot;
   * but they are tiny.) */
  mutex_name /= 'u';
  mutex_name += to_string(m_srv_app_ref.m_user_id);
  mutex_name += 'g';
  mutex_name += to_string(m_srv_app_ref.m_group_id);
  mutex_name += 'p';
  mutex_name += to_string(int(m_srv_app_ref.m_permissions_level_for_client_apps));

  return mutex_name;
} // Session_base::cur_ns_store_mutex_absolute_name()

TEMPLATE_SESSION_BASE
fs::path CLASS_SESSION_BASE::cur_ns_store_absolute_path() const
{
  using Path = fs::path;

  Path path(m_srv_app_ref.m_kernel_persistent_run_dir_override.empty()
              ? util::IPC_KERNEL_PERSISTENT_RUN_DIR
              : m_srv_app_ref.m_kernel_persistent_run_dir_override);
  path /= m_srv_app_ref.m_name;
  path += ".libipc-cns.pid";
  return path;
}

TEMPLATE_SESSION_BASE
Shared_name CLASS_SESSION_BASE::session_master_socket_stream_acceptor_absolute_name() const
{
  assert((!m_srv_namespace.empty()) && "Serv-namespace must have been set by set_srv_namespace() by now.");

  // Global scope.  Pertains to Server_app::m_name, the particular process, *all* Client_apps, *all* sessions.
  auto acc_name = build_conventional_shared_name(transport::Native_socket_stream_acceptor::S_RESOURCE_TYPE_ID,
                                                 Shared_name::ct(m_srv_app_ref.m_name),
                                                 m_srv_namespace);
  /* That was the standard stuff.  Now the us-specific stuff:
   *
   * Above we specified it's a socket stream acceptor; but which one/for what?  It's the required one for
   * any further IPC; hence S_SENTINEL, `0`.  (As of this writing, in ipc::session anyway, we don't have any other use
   * for Native_socket_stream_acceptor (other socket streams are opened through the ones birthed through this one).
   * If we did, we could use `1`, `2`, etc., or something.  Anyway Shared_name::S_SENTINEL here is a solid choice.) */
  acc_name /= Shared_name::S_SENTINEL;

  return acc_name;
} // Session_base::session_master_socket_stream_acceptor_absolute_name()

TEMPLATE_SESSION_BASE
typename CLASS_SESSION_BASE::Structured_msg_builder_config
  CLASS_SESSION_BASE::heap_fixed_builder_config(flow::log::Logger* logger_ptr) // Static.
{
  using transport::sync_io::Native_socket_stream;
  using transport::struc::Heap_fixed_builder;

  /* This is arguably easiest when one has an actual Channel_obj; see struc::Channel::heap_fixed_builder_config()
   * which works with that.  Then one can query the Channel_obj for its send-blob/meta-blob-max-size(s), pick the
   * smaller, and return that.  We are static -- the whole point is to still work when one doesn't have an actual
   * channel but might still want to construct a transport::struc::Msg_out -- in fact they might not even have a Session
   * in our case.  Not a big deal: we use similar logic but have to use some specific knowledge about how
   * we use our own class template params (S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES) to configure any channels
   * we open; this tells us how big the messages the various pipes (we ourselves set up) can send-out.
   *
   * So follow along that other method's (rather simple) procedure and apply our "insider" knowledge.  Namely:
   * We will *specifically* either use an MQ (of one of 2 specific types), or a socket stream, or both.
   * - If socket stream *only*: Then Native_socket_stream advertises its blob/meta-blob max-size in a
   *   non-concept-implementing public constant.  Use that; simple.
   * - If MQ *only*: Not quite as simple; the message max size is configurable at run time, and we don't have
   *   the actual MQ object (by definition).  Not to worry:
   *   - Suppose we're really Server_session_impl.  Yay: we are the ones, in Server_session_impl::make_channel_mqs(),
   *     who determine the MQ size, for any channel that is opened.  It's a constant... so use that constant!
   *   - Suppose we're really Client_session_impl.  That's slightly annoying, as the opposing Server_session_impl is
   *     technically determining the MQ size (prev bullet).  As of this writing, though, it's always been a constant
   *     which has never changed.  So we can just use that same constant.  In the future, *technically*, the
   *     procedure could change -- but we punt on dealing with until such time that's an actual worry.
   *     The constant's doc header has notes about it, so it won't be overlooked.  (Spoiler alert: It's unlikely
   *     there will be a pressing need to mess with this; SHM-based transmission obviates the need for such
   *     perf tweaking.)
   * - If both: Use the lower of the two (as as in struc::Channel::heap_fixed_builder_config()). */

  size_t sz;
  if constexpr(S_MQS_ENABLED && (!S_SOCKET_STREAM_ENABLED))
  {
    sz = S_MQS_MAX_MSG_SZ;
  }
  else if constexpr((!S_MQS_ENABLED) && S_SOCKET_STREAM_ENABLED)
  {
    sz = Native_socket_stream::S_MAX_META_BLOB_LENGTH;
  }
  else
  {
    static_assert(S_MQS_ENABLED && S_SOCKET_STREAM_ENABLED, "There must be *some* transport mechanism.");

    sz = std::min(S_MQS_MAX_MSG_SZ, Native_socket_stream::S_MAX_META_BLOB_LENGTH);
  }

  return Heap_fixed_builder::Config{ logger_ptr, sz, 0, 0 };
} // Session_base::heap_fixed_builder_config()

TEMPLATE_SESSION_BASE
typename CLASS_SESSION_BASE::Structured_msg_reader_config
  CLASS_SESSION_BASE::heap_reader_config(flow::log::Logger* logger_ptr) // Static.
{
  return transport::struc::Heap_reader::Config{ logger_ptr, 0 };
} // Session_base::heap_reader_config()

#undef CLASS_SESSION_BASE
#undef TEMPLATE_SESSION_BASE

} // namespace ipc::session
