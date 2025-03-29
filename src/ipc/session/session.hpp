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
#include "ipc/session/schema/common.capnp.h"
#include "ipc/transport/struc/heap_serializer.hpp"
#include <experimental/propagate_const>

namespace ipc::session
{

// Types.

#ifdef IPC_DOXYGEN_ONLY // Not compiled: for documentation only.  Contains concept docs as of this writing.

/**
 * A documentation-only *concept* defining the local side of an IPC conversation (*session*)
 * with another entity (typically a separate process), also represented by a Session-implementing object,
 * through which one can easily open IPC channels (ipc::transport::Channel), among other IPC features.
 * While it is possible to open a `Channel` without the help of this Session concept, for many users writing
 * large multi-channel applications it would be too laborious to do so.  The Session concept is the central
 * one in ipc::session.
 *
 * @see ipc::session doc header.
 * @see Session_mv which implements the majority of Session concept, namely when it is in PEER state.
 *
 * ### Concept contents ###
 * The concept defines the following behaviors/requirements.
 *   - The object has at least 2 states:
 *     - NULL: Object is not a peer: is not open/capable of operation.  Essentially it is not a useful
 *       state.  A default-cted object is in NULL state; and a moved-from object is as-if default-cted, therefore
 *       also in a NULL state.  In this state all the transmission-related methods return `false`/sentinel and no-op.
 *     - PEER: Object is or has been a peer (open/capable of operation).  It is not possible to exit PEER
 *       state, except via being moved-from.  The session ending, such as due to the partner process exiting,
 *       retains PEER state for instance.
 *     - Other states are allowed but outside the scope of the concept.  For example a CONNECTING state may or may
 *       not be relevant (but again not relevant to this concept).
 *   - Active-opening a channel: open_channel().  See its doc header.  It yields a PEER-state #Channel_obj.
 *   - Passive-opening a channel (the counterpart to the partner Session's open_channel()).
 *     - At entry to PEER state (the mechanism of accomplishing which, as noted below, is not specified by the concept)
 *       the user must have provided a passive-open handler (discussed below in detail) via which subsequent
 *       passive-opens are reported to the user.  To this handler a PEER-state #Channel_obj is passed in case of
 *       success.
 *     - Alternatively, if at entry to PEER state the user has chosen to *not* supply a passive-open handler,
 *       then the partner Session::open_channel() shall yield failure.  In this mode of operation, the user has
 *       chosen this side of the session to not act as a passive-opener.  (If both `Session`s do so, the session
 *       is essentially useless.  Formally this is supported, simply because there is no need to enforce otherwise.)
 *   - Async error reporting: At entry to PEER state (the mechanism of accomplishing which, as noted below, is not
 *     specified by the concept) the user must have provided an on-error handler via which asynchronously-occurring
 *     session-hosing #Error_code are reported.  See details below.
 *   - The (outgoing) transmission-of-messages methods, including transmission of graceful-close message.
 *     See their doc headers.
 *   - Behavior when the destructor is invoked.  See ~Session() doc header.
 *   - Default ctor (which creates a NULL-state object that can be moved-to and to which "as-if"
 *     state any moved-from object is changed).
 *   - Move ctor, move assigment operator; these do the reasonable thing including setting the moved-from object
 *     to NULL state.  Various satellite APIs (e.g., Session_server) may
 *     need these.  That is such APIs do not rely on the factory/shared-ownership pattern.
 *
 * The concept (intentionally) does *not* define the following behaviors:
 *   - How to create a Session, except the default and move ctors.
 *     That is it does not define how to create a *new* (not moved-from) and *functioning* (PEER state:
 *     open/capable of communication) peer object.  It only defines behavior once the user has access to
 *     a Session that is already in PEER state: connected to an opposing peer Session.
 *
 * ### Async-I/O channels versus `sync_io` ones ###
 * There are 3, overall, ways to obtain a transport::Channel via ipc::session.
 *   - Session::open_channel().  It's a synchronous operation; it sets a `Channel` as a pointer out-arg.
 *   - Passive-opening.  It's an async operation via passive-open handler (see below for details).
 *   - Init-channels.  Client_session_mv::sync_connect() (et al) and Session_server::async_accept() (et al)
 *     provide ways to pre-open channels to be ready the moment the session enters PEER state.
 *
 * In all cases these channels shall bear `sync_io`-pattern peer objects: not async-I/O ones.
 *   - They are lighter-weight to create and pass around.
 *   - They can be immediately fed to transport::struc::Channel or transport::struc::sync_io::Channel ctor
 *     via `move()` semantics.
 *   - They can be trivially upgraded to async-I/O form via transport::Channel::async_io_obj().
 *
 * ### Thread safety and handler invocation semantics ###
 * As is typical: 2 different Session objects are safe to invoke methods on concurrently; it is not safe to
 * invoke a non-`const` method on a given `*this` concurrently to any other method on the same `*this`.
 *
 * The handlers -- namely the passive-open handler (optional) and the on-error handler (mandatory) -- shall be
 * invoked from some unspecified thread that is not any one of the user's calling threads.  It is *not* allowed
 * to invoke `*this` APIs from within any such handler.  For example, you may not open_channel() from
 * the passive-open handler.  Informally, it is recommended that any user handler post its actual handling of the
 * event onto their own thread(s), such as via `flow::async::Single_thread_task_loop::post()`.
 *
 * ### Passive-open handler semantics ###
 * As summarized above, a Session -- at entry to PEER state -- is optionally capable of accepting the partner
 * Session's active-open (open_channel()) attempt.  The user must make the decision whether to be capable or
 * incapable of performing such a passive-open.  If they decide the latter, they must not set the *on-passive-open*
 * handler; else they must set it by the time PEER state begins.  (The mechanism of doing so is unspecified by
 * the concept.  The current impls, #Server_session and #Client_session, use different APIs for this.)
 *
 * If the choice is made to indeed perform passive-opens (accept other side's active-opens), then suppose the
 * handler H is indeed saved inside `*this` (which is in PEER state).  Then the following shall be invoked from some
 * unspecified thread that is not one of the user's public API-invoking threads: `H(C, M)`, where `C` is a
 * #Channel_obj onto which the new PEER-state transport::Channel shall be `move()`d; while `M` is the metadata reader
 * (explained below).  Therefore the H signature must be compatible with:
 *
 *   ~~~
 *   void H(Channel_obj&& new_peer_channel, Mdt_reader_ptr&& metadata_reader)
 *   ~~~
 *
 * Note that #Mdt_reader_ptr is a `shared_ptr` to a capnp-generated `Reader` to a capnp-`struct`
 * parameterized on #Mdt_payload_obj.  The memory resources taken by the serialization of the metadata
 * (including certain internal zero-copy "baggage") shall be freed once that `shared_ptr` ref-count group is empty.
 * As for what the heck this even *is*: see open_channel()'s metadata arg docs.  This is the `Reader` counterpart
 * to the `Builder` mutated by the opposing open_channel() caller.
 *
 * ### Open channel metadata ###
 * open_channel() is used to initiate a channel opening on one side, and the on-passive-open handler is used
 * to accept the same on the other side (for a given channel).  The result, on each side, is a #Channel_obj,
 * which is a `Channel` template instantiation.  Note that it is not a `struc::Channel`; the user may choose
 * to immediate convert `Channel` to `struc::Channel` via std::move(), but whether they want to or not is up
 * to them and their desired use case.  Now, there are millions of patterns of what the acceptor might want
 * to accept in a session; there could be one channel for some app-wide updates and one per processor core;
 * and possibly more differentiation than that regarding which channel is for what purpose.  Meanwhile the only
 * way to know which channel is being opened, other than a set ordering (certainly inconvenient at best in many
 * cases), is to perhaps exchange some information on the channels themselves.  That too is inconvenient:
 * even having discovered that a certain channel is for a certain purpose, distributing them around the app is
 * no fun probably.  Furthermore, it's only a `Channel`: is the user supposed to encode something in a binary
 * blob?  Inconvenient.  But if one turns it into a `struc::Channel`, then that is irreversible and may not
 * be what the user wants.  The solution we use: the open_channel() call is issued with a separate (to any
 * structured in-message the user would see) chunk of capnp-structured *metadata*.  This is transmitted
 * and supplied to the on-passive-open handler per the preceding doc section.  Details:
 *
 * It is encoded in a capnp-`struct` `Metadata`, defined in common.capnp.  However that `struct`
 * is generically parameterized on another capnp-type of the user's choice.  That is the Session template arg
 * #Mdt_payload_obj.  Hence the user shall decide how to encode the channel-differentiating info on a
 * per-Session basis.  capnp shall generate the usual `Reader` and `Builder` nested classes; the latter to
 * build it to pass into open_channel(); the former to read it, the on-passive-open handler having fired.
 * Call capnp-generated `Builder` setters and `Reader` getters accordingly on either side.
 *
 * This feature is optional to use.  You may set #Mdt_payload_obj to `capnp::Void`; omit
 * the metadata argument to open_channel(); and ignore the `Reader` counterpart in the on-passive-open handler.
 *
 * Informal suggestion: While Session concept does not specify anything about clients or servers, as of this
 * writing the client-server model is the only one available, hence #Client_session and #Server_session are the impls
 * used.  `Server_session`s are produced, in this model, via Session_server.  The latter class template
 * requires that `Mdt_payload` be specified as a template arg to be applied to all Server_session
 * objects it produces.  Therefore it must be decided not just for each session but realistically for the entire
 * client/server split (in other words the entire meta-application that was split into two): all the
 * `Server_session`s; and therefore to all the `Client_session`s as well (since the 2 are peers to each other).
 * So the method of signaling channel type (if needed) via metadata is quite a high-level decision.
 * One obvious idea is to maintain a split-wide capnp schema that specifies the `struct` to use as
 * `Mdt_payload`; in that `struct` define perhaps a capnp-`enum` type for channel type.
 * Alternatively one can define a top-level anon capnp-`union`; its `which()` selector can function as an
 * enumeration (in fact it is one in the generated C++); and any further data (e.g., the processor core index,
 * if there's some channel-per-core setup) inside that particular `which()`'s `struct`.  Do be careful to
 * (1) maintain backward and forward compatibility as channel types are added over time; and (2)
 * do not abuse the metadata store with large structures.  Anything that is not directly used in differentiating
 * between channels should simply be communicated over the channel itself, once it has been routed to the
 * proper area of code; that's what transport::struc::Channel is for in particular.
 *
 * ### Error handling semantics ###
 * There are 2 sources of errors: background (async) errors and synchronously from invoking active-open: open_channel().
 * A given error is either *session-hosing* (fatal) or not.  To make handling error simpler these are the semantics:
 *   - open_channel() synchronously emitted errors: *not* session-hosing.
 *   - Background (async) errors: *always* session-hosing.
 *
 * Even if, internally, open_channel() in some way triggers a session-hosing error, it shall be emitted
 * as-if it were an async error in a way indistinguishable in practice from any other async error.
 *
 * When a session-hosing error occurs, it is emitted exactly once (through the on-error handler).  After such an
 * event, open_channel() and other APIs shall no-op/return sentinel subsequently if invoked.
 *
 * This is simpler than how transport::struc::Channel emits errors; there the (always synchronous) `send()`
 * can emit an error synchronously that is session-hosing; in addition to incoming-direction session-hosing errors;
 * but still only one of these can occur and only once.  So in that sense open_channel() is *not* like `send()`:
 * if it emits an error, `*this` is still formally in non-session-hosed state.
 *
 * However, of course, the user is free to treat an open_channel()-emitted error as fatal and destroy `*this`.
 *
 * @see Channel and buddies: the main object of opening a Session is to be able to easily active-open and/or
 *      passive-open `Channel`s in PEER state through it.
 * @see #Client_session, #Server_session: as of this writing, any non-SHM-enabled pair of PEER-state `Session`s is
 *      a #Client_session and a #Server_session.  `Server_session`s in a process, designated as the *server* in an
 *      IPC *split*, are obtained (following the acceptor pattern) via a single Session_server in that
 *      process.  On the other side, a Client_session is directly constructed (in NULL state) and
 *      enters PEER state via Client_session::sync_connect().  (Conceptually this is similar to the relationship
 *      between, e.g., Native_socket_stream (server side), Native_socket_stream_acceptor, and Native_socket_stream
 *      (client side).  However, it so happens that in the case of `Session`s, a different class is used depending
 *      on which side the Session sits.  Since they both implement the same concept, however, code can be written
 *      that behaves identically -- given any Session in PEER state -- regardless of whether it's really a
 *      Client_session or Server_session.  That said, such code does have to be templated on the particular
 *      Session as a template param.)
 * @see E.g., shm::classic::Client_session and shm::classic::Server_session which are an example of a SHM-enabled
 *      variant of Session impls.
 *
 * @tparam Mdt_payload
 *         capnp-generated class for the user's capnp-`struct` of choice for open_channel() transmitting metadata
 *         information to be received on the other side's on-passive-open handler.  See discussion above.
 *         Use `capnp::Void` if not planning to use this metadata feature.
 */
template<typename Mdt_payload = ::capnp::Void>
class Session
{
public:
  // Types.

  /**
   * Each successful open_channel() and on-passive-open handler firing shall yield a concrete transport::Channel
   * instance of this type -- in PEER state.  The concept does not specify how the concrete Channel instance type --
   * meaning the template params to `Channel` -- is determined.  In practice it is likely to be controlled by
   * unspecified compile-time knobs, likely additional template params, to the particular Session impl.
   * E.g., see Client_session:S_MQS_ENABLED.
   *
   * In practice a user is likely to declare `Channel` variables by using this alias (or via `auto`).
   *
   * @note #Channel_obj shall be such that its compile-time transport::Channel::S_IS_SYNC_IO_OBJ is `true`.
   *       E.g., if it's got a native-socket-stream, it'll be transport::sync_io::Native_socket_stream -- not
   *       transport::Native_socket_stream.  If you want one with transport::Channel::S_IS_ASYNC_IO_OBJ then
   *       use transport::Channel::async_io_obj() (or to just get the type at compile time:
   *       transport::Channel::Async_io_obj).
   */
  using Channel_obj = unspecified;

  /// Short-hand for `Mdt_payload` template arg.
  using Mdt_payload_obj = Mdt_payload;

  /// Pointee of #Mdt_builder_ptr.
  using Mdt_builder
    = typename transport::struc::schema::Metadata<Mdt_payload_obj>::Builder;

  /**
   * Ref-counted handle to a capnp-generated `Builder` (and the payload it accesses) through which the user
   * shall mutate the open_channel() metadata before invoking open_channel().  mdt_builder() returns
   * a new one; then user mutates it; then user calls open_channel().  To mutate an `x` of this type:
   * `auto root = x->initPayload();`, then mutate via the `root` sub-builder (standard capnp-generated API for whatever
   * fields capnp-`struct` #Mdt_payload_obj defines).
   */
  using Mdt_builder_ptr
    = shared_ptr<Mdt_builder>;

  /**
   * Ref-counted handle to a capnp-generated `Reader` (and the payload it accesses) through which the user
   * shall access the metadata the other side prepared via #Mdt_builder_ptr.  On-passive-open handler
   * yields a handle of this type.  To access via an `x` of this type:
   * `auto root = x->getPayload();`, then access via the `root` sub-reader (standard capnp-generated API for whatever
   * fields capnp-`struct` #Mdt_payload_obj defines).
   */
  using Mdt_reader_ptr
    = shared_ptr<typename transport::struc::schema::Metadata<Mdt_payload_obj>::Reader>;

  /**
   * Convenience alias for the transport::struc::Channel concrete type to use if one desires (1) to upgrade a
   * `*this`-generated channel to a `struc::Channel` and (2) to efficiently use the built-in
   * capabilities of `*this` (notably, if applicable: SHM) for zero-copy performance when sending messages through
   * that channel.
   *
   * @note This alias is to an async-I/O `struc::Channel`.  (See util::sync_io doc header for background on
   *       the 2 patterns, async-I/O and `sync_io`.)  To obtain the `sync_io`-core counterpart type use:
   *       `Session::Structured_channel::Sync_io_obj`.
   *
   * ### Informal suggestion ###
   * Use this (parameterized by `Message_body` of choice of course) as your transport::struc::Channel
   * concrete type.  Then use the appropriate tag-form `struc::Channel` ctor.  Namely as of this writing:
   *   - `*_shm_*_session`: Use tag
   *     transport::struc::Channel_base::S_SERIALIZE_VIA_APP_SHM or
   *     transport::struc::Channel_base::S_SERIALIZE_VIA_SESSION_SHM
   *   - Non-shm `*_session`: Use tag
   *     transport::struc::Channel_base::S_SERIALIZE_VIA_HEAP.
   *
   * @tparam Message_body
   *         See transport::struc::Channel.  All the other annoying decisions are made for you using this alias;
   *         but of course you must still specify the language you shall be speaking over the channel.
   */
  template<typename Message_body>
  using Structured_channel
    = transport::struc::Channel<unspecified>;

  /**
   * Convenience alias equal to `Structured_channel<M>::Builder_config` (regardless of `M`).
   * See #Structured_channel.
   *
   * Informally: The impl template may choose to provide methods to obtain a #Structured_msg_builder_config
   * to use for a `Structured_channel::Msg_out` that can be constructed without a concrete #Structured_channel
   * available.  These methods might in some cases even be `static` (in which case they should
   * take a `Logger* logger_ptr` 1st arg), so that not even having a `Session` object in hand is needed.
   */
  using Structured_msg_builder_config = unspecified;

  /**
   * Convenience alias equal to `Structured_channel<M>::Reader_config` (regardless of `M`).
   * See #Structured_channel.  The utility of explicit use of this alias by the user is less than
   * #Structured_msg_builder_config, as a transport::struc::Msg_in is not constructed directly
   * but by transport::struc::Channel internals.
   */
  using Structured_msg_reader_config = unspecified;

  // Constants.

  /**
   * Specifies the SHM-provider for which this `Session` concept implementation provides support; or `NONE`
   * if this is a vanilla `Session` that does not provide SHM-based zero-copy support.  May be useful for
   * generic programming; perhaps in `std::conditional` or `if constexpr()` uses.  Informally we recommend against
   * frequent use of this except logging/reporting/alerting and debug/test scenarios.
   *
   * @see #S_SHM_ENABLED.
   */
  static constexpr schema::ShmType S_SHM_TYPE = unspecified;

  /**
   * Specifies whether this `Session` concept implementation provides support for zero-copy via SHM plus
   * direct access to SHM arenas and lending/borrowing within them.  May be useful for
   * generic programming; perhaps in `std::conditional` or `if constexpr()` uses.
   *
   * @see #S_SHM_TYPE
   */
  static constexpr bool S_SHM_ENABLED = unspecified;

  /**
   * Specifies whether `Session`s of this type are designated as the server side (`true`) or client side (`false`).
   * May be useful for generic programming, particularly when already in PEER state: the two sides at that point
   * have symmetrically equal abilities, but it can still be useful to use this value as a side ID.  That is, at
   * that point the user can consider this as an "address" in a 2-node system: this is either node `true` or
   * node `false`, even though as black boxes both nodes are capable of the same stuff: but you might want to decide
   * which side will *use* those abilities one way versus another way.
   *
   * @note While not the case *as of this writing* (wherein to enter PEER state a `Session` must connect as a client
   *       or be yielded from a `Session_server`), it is conceivable a `Session` impl can begin life immediately in
   *       PEER state, e.g., based on a pre-connected bootstrap comm channel.  In that case still one side shall
   *       be assigned a distinct "address" via this value, even if it has little or nothing to do with servers
   *       and clients.  (By analogy: one can construct a transport::Native_socket_stream from a pre-connected
   *       native-handle, on each side, so no client or server procedure was necessarily involved to obtain those
   *       native-handles; it might have been `connect_pair()` instead; yet one could still assign one of those sides
   *       as the "server," hence the other is the "client.")
   */
  static constexpr bool S_IS_SRV_ELSE_CLI = unspecified;

  // Constructors/destructor.

  /**
   * Default ctor: creates Session in NULL state (not operational).
   * Beyond this and the move ctor, other ctors are unspecified by the concept but may well exist.  See discussion in
   * concept doc header.
   *
   * The practical intended use for this ctor is as a target for a subsequent move-to assignment.  For example,
   * Session_server::async_accept() would typically be given a default-cted Server_session which, on success,
   * is moved-to to enter PEER state.
   */
  Session();

  /**
   * Move ctor: `*this` becomes identical to `src`; while `src` becomes as-if default-cted.
   *
   * @param src
   *        Moved-from session that enters NULL state.
   */
  Session(Session&& src);

  /// Copy construction is disallowed.
  Session(const Session&) = delete;

  /**
   * In NULL state, no-op; in PEER state: Ends the session while informing (if possible) the opposing peer Session that
   * the present session has ended.  This may incur a short delay before the dtor exits (in practice likely to
   * flush out any pending sends over an internal session master channel, informally speaking).
   *
   * Any #Channel_obj previously yielded by open_channel() and/or passive-opens shall continue operating, if still
   * operating, and it is up to the user to perform any cleanup/flushing/etc. on these.  This is consistent with
   * the idea that open_channel() and passive-open passes ownership of such channels to the user.
   *
   * There are 2 mutually exclusive expected triggers for why this dtor would be invoked:
   *   - The present process is exiting gracefully (hence the session must end).  User invokes dtor.  Dtor informs
   *     the other side with maximum expediency; then returns.
   *   - The opposing process is exiting gracefully or otherwise; the opposing Session thus informed ours gracefully
   *     or otherwise; therefore `*this` informs user of this via emitted error (see concept doc header regarding
   *     session-hosing error emission semantics); therefore `*this` is no longer usable; hence user invokes
   *     `*this` dtor.  It tries to inform the other side, but there's nothing to inform; it quickly returns.
   */
  ~Session();

  // Methods.

  /**
   * Move assignment: acts as-if `*this` dtor executed; then `*this` becomes identical to `src`; while `src` becomes
   * as-if default-cted.  No-op if `&src == this`.
   *
   * @param src
   *        Moved-from session that enters NULL state (unless `&src == this`).
   * @return `*this`.
   */
  Session& operator=(Session&& src);

  /// Copy assignment is disallowed.
  Session& operator=(const Session&) = delete;

  /**
   * Returns a new metadata holder to be subsequently mutated by the user and then passed to open_channel() to
   * initiate active-open attempt.  The payload (see #Mdt_payload_obj) is left uninitialized:
   * the user would likely `x->initPayload()` and then mutate stuff inside that (where `x` was returned).
   *
   * If `*this` is not in PEER state, returns null.
   *
   * This call is not needed, if one plans to use open_channel() overload that takes no metadata.
   *
   * @return See above.
   */
  Mdt_builder_ptr mdt_builder();

  /**
   * Synchronously active-opens a new channel which, on success, is moved-to `*target_channel`.
   * No-op and return `false` if `*this` is not in PEER state, the session is hosed by a previous error,
   * or `mdt` is null.  Else returns `true`.
   *
   * `mdt` shall be from an mdt_builder() call, and `mdt` must not have been passed to
   * open_channel() previously; otherwise behavior undefined.  See also the open_channel() overload that takes no `mdt`.
   *
   * @note This is a compile-time fact, but we point it out just the same: `*target_channel` is of
   *       a `sync_io`-core-bearing type.  However, on successful open_channel() you can trivially obtain
   *       an async-I/O version via `target_channel->async_io_obj()`.
   *
   * ### Is it blocking or non-blocking? ###
   * That's a matter of perspective; but formally it must be either non-blocking or, if blocking, then limited to
   * a timeout no greater than seconds (not minutes).  That said, informally, the implementation shall strive to make
   * this quite fast, enough so to be considered non-blocking, *as long as both Session objects are in PEER state*.
   *
   * ### Error emission ###
   * If `false` is returned, even if open_channel() internally triggered a session-hosing condition, it shall be
   * fired through the on-error handler and not emitted by open_channel().
   *
   * If `true` is returned an error may be detected and is then emitted via standard Flow semantics
   * (if `err_code` then set `*err_code` to error; otherwise throw).  Such an error is *not* session-hosing.
   *
   * In particular if passive-opens are disabled on the opposing side it must emit
   * error::Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN.  Informally: an impl is likely to
   * be capable of emitting the non-fatal error::Code::S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT, though the user
   * is reasonably likely to want to treat it as fatal in practice and end the session.
   *
   * @param target_channel
   *        On success `*target_channel` is moved-to from a (new) #Channel_obj in PEER state.
   * @param mdt
   *        See mdt_builder().  If `mdt` is null, open_channel() returns `false`.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        error::Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN,
   *        other possible unspecified errors according to impl's discretion.  See above discussion.
   * @return See above.
   */
  bool open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt, Error_code* err_code = 0);

  /**
   * Identical to `open_channel(target_channel, mdt_builder(), err_code)`; in other words
   * attempts to open channel with an uninitialized channel-open metadata payload.  Informally: this overload
   * can be used if differentiation among channels, for the passive-open side's benefit, is not necessary.
   * In that case #Mdt_payload_obj is typically set to `capnp::Void`.
   *
   * @note This is a compile-time fact, but we point it out just the same: `*target_channel` is of
   *       a `sync_io`-core-bearing type.  However, on successful open_channel() you can trivially obtain
   *       an async-I/O version via `target_channel->async_io_obj()`.
   *
   * @param target_channel
   *        See other open_channel().
   * @param err_code
   *        See other open_channel().
   * @return See other open_channel().  However it cannot return `false` due to an empty `mdt`, as `mdt`
   *         is internally generated (and left uninitialized).
   */
  bool open_channel(Channel_obj* target_channel, Error_code* err_code = 0);

  /**
   * In PEER state: Returns the (non-nil) logged-in session token to be used for any `struc::Channel`s
   * one may wish to upgrade from #Channel_obj channels yielded by open_channel() or passive-opens;
   * or nil if in NULL state, or if `*this` session is hosed from a prior error.
   *
   * Informal context: Firstly see transport::struc::Channel::session_token() and class template doc header.
   * However note that open_channel() (active-open) and on-passive-open handler yields an unstructured, unused
   * #Channel_obj: it is entirely optional that the user then upgrade it to a `struc::Channel`.  *If* one
   * does so, however, then a non-nil session-token shall be *required* and, for safety, must equal
   * what this session_token() returns.
   *
   * @return See above.  The reference returned shall always be one of 2: a certain unspecified internal
   *         item, or to transport::struc::NULL_SESSION_TOKEN.
   */
  const Session_token& session_token() const;
}; // class Session

#endif // IPC_DOXYGEN_ONLY

/**
 * Implements the Session concept when it is in PEER state.  It is, as of this writing, not to be instantiated
 * by itself.  Rather see Client_session_mv and Server_session_mv regarding what to actually instantiate and
 * further context.  As for the API in Session_mv itself, it is quite critical -- but it only implements
 * Session concept, and therefore the doc headers for Session_mv members generally just send you to
 * Session doc headers.  However the Session_mv doc headers are useful in enumerating specific #Error_code
 * values that may be emitted.
 *
 * @internal
 * ### Implementation ###
 * See pImpl-lite notes in Blob_stream_mq_sender doc header.  The exact same applies here.
 *
 * Furthermore, sub-classes -- Client_session_mv, Server_session_mv -- continue the pImpl-lite pattern
 * for their added public APIs on top of the ones from this super-class.  For this reason impl() is `protected`.
 * E.g., Client_session_mv doc header explains this hierarchy.  In particular it explains what
 * `Session_impl_t` is to be set to properly.
 *
 * @endinternal
 *
 * @tparam This is an implementation detail.  See Client_session_mv and Server_session_mv doc headers for
 *         how to easily set this to the proper type in a given situation.
 */
template<typename Session_impl_t>
class Session_mv
{
protected:
  // Types.

  /// Short-hand for pImpl-lite impl type.  This shall be the deepest impl sub-class desired.
  using Impl = Session_impl_t;

public:
  // Types.

  /**
   * Implements Session API per contract.
   * @see Session::Channel_obj: implemented concept.
   */
  using Channel_obj = typename Impl::Channel_obj;

  /// Container (`vector<>`) of #Channel_obj.
  using Channels = typename Impl::Channels;

  /**
   * Implements Session API per contract.
   * @see Session::Mdt_payload_obj: implemented concept.
   */
  using Mdt_payload_obj = typename Impl::Mdt_payload_obj;

  /**
   * Implements Session API per contract.
   * @see Session::Mdt_builder: implemented concept.
   */
  using Mdt_builder = typename Impl::Mdt_builder;

  /**
   * Implements Session API per contract.
   * @see Session::Mdt_builder_ptr: implemented concept.
   */
  using Mdt_builder_ptr = typename Impl::Mdt_builder_ptr;

  /**
   * Implements Session API per contract.
   * @see Session::Mdt_reader_ptr: implemented concept.
   */
  using Mdt_reader_ptr = typename Impl::Mdt_reader_ptr;

  /**
   * Implements Session API per contract.
   *
   * @note In particular Client_session_mv and Server_session_mv inherit this, and thus they show up there and thus
   *       in #Client_session and #Server_session.  However if you're using a SHM-enabled
   *       variant (e.g., shm::classic::Client_session, shm::classic::Server_session), then
   *       this alias will be shadowed by a better definition that will make use of SHM for zero-copy transmission
   *       of structured messages.  This super-class definition then becomes, in a sense, obsolete:
   *       It can be used, if you still want to use non-zero-copy heap-based serialization; but it is usually
   *       better to use, directly, the shadowing definition: e.g., shm::classic::Session_mv::Structured_channel
   *       (which will show up in shm::classic::Server_session and shm::classic::Client_session).
   *
   * @see Session::Structured_channel: implemented concept.
   */
  template<typename Message_body>
  using Structured_channel
    = typename transport::struc::Channel_via_heap<Channel_obj, Message_body>;

  /**
   * Implements Session API per contract.
   *
   * @note Same note as in #Structured_channel doc header.
   *
   * @see Session::Structured_msg_builder_config: implemented concept.
   */
  using Structured_msg_builder_config = typename Impl::Structured_msg_builder_config;

  /**
   * Implements Session API per contract.
   *
   * @note Same note as in #Structured_channel doc header.
   *
   * @see Session::Structured_msg_reader_config: implemented concept.
   */
  using Structured_msg_reader_config = typename Impl::Structured_msg_builder_config;

  // Constants.

  /**
   * Implements Session API per contract.
   * @see Session::S_SHM_TYPE: implemented concept.
   */
  static constexpr schema::ShmType S_SHM_TYPE = Impl::S_SHM_TYPE;

  /**
   * Implements Session API per contract.
   * @see Session::S_SHM_ENABLED: implemented concept.
   */
  static constexpr bool S_SHM_ENABLED = Impl::S_SHM_ENABLED;

  /// Compile-time-known constant indicating whether #Channel_obj shall use a blobs pipe over message queues (MQs).
  static constexpr bool S_MQS_ENABLED = Impl::S_MQS_ENABLED;

  /**
   * Compile-time-known constant indicating whether #Channel_obj shall use socket stream for any type of pipe.
   * If #S_MQS_ENABLED, then a socket stream is still needed if and only if user wants to transmit native handles.
   * If #S_MQS_ENABLED is `false`, then a socket stream is the only transport available as of this writing and hence
   * also enabled.
   */
  static constexpr bool S_SOCKET_STREAM_ENABLED = Impl::S_SOCKET_STREAM_ENABLED;

  // Constructors/destructor.

  /**
   * Implements Session API per contract.  Reminder: While not enforced, this ctor form is informally intended only
   * for when one will move-from a PEER-state Session_mv.  In particular Session_server::async_accept()
   * requires these move semantics; so a default-cted Session_mv would be typically passed to it by the
   * user.
   *
   * @see Session::Session(): implemented concept.
   */
  Session_mv();

  /**
   * Implements Session API per contract.
   *
   * @param src
   *        See above.
   *
   * @see Session move ctor: implemented concept.
   */
  Session_mv(Session_mv&& src);

  /// Copy ction is disallowed.
  Session_mv(const Session_mv&) = delete;

  /**
   * Implements Session API per contract.
   * @see Session::~Session(): implemented concept.
   */
  ~Session_mv();

  // Methods.

  /**
   * Implements Session API per contract.
   *
   * @param src
   *        See above.
   * @return See above.
   *
   * @see Session move assignment: implemented concept.
   */
  Session_mv& operator=(Session_mv&& src);

  /// Copy assignment is disallowed.
  Session_mv& operator=(const Session_mv&) = delete;

  /**
   * Implements Session API per contract.
   *
   * @return See above.
   *
   * @see Session::mdt_builder(): implemented concept.
   */
  Mdt_builder_ptr mdt_builder();

  /**
   * Implements Session API per contract.  Reminder: Per concept requirements, open_channel() emitting an error
   * (via standard Flow semantics) means that error is *not* session-hosing.  Any session-hosing event is orthogonally
   * emitted to on-error handler.  Informally: in most cases even the (formally non-session-hosing) errors
   * below indicate something seriously wrong is afoot.  It would not be unreasonable to treat them as session-hosing
   * (invoke dtor).
   *
   * @param target_channel
   *        See above.
   * @param mdt
   *        See above.
   * @param err_code
   *        See above.  #Error_code generated:
   *        error::Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN (per concept),
   *        error::Code::S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT (peer neither accepted nor rejected in non-blocking
   *        time frame),
   *        error::Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE (opposing or local
   *        #Server_session peer was unable to acquire resources -- as of this writing MQ-related ones -- required
   *        for opening channel).
   * @return See above.
   *
   * @see Session::open_channel(): implemented concept.
   */
  bool open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt, Error_code* err_code = 0);

  /**
   * Implements Session API per contract.  See other overload for brief discussion relevant here also.
   *
   * @param target_channel
   *        See above.
   * @param err_code
   *        See above.  See also other overload.
   * @return See above.
   *
   * @see Session::open_channel(): implemented concept.
   */
  bool open_channel(Channel_obj* target_channel, Error_code* err_code = 0);

  /**
   * Implements Session API per contract.
   *
   * @return See above.
   *
   * @see Session::session_token(): implemented concept.
   */
  const Session_token& session_token() const;

  /**
   * Utility that obtains a heap-based (non-zero-copy) Struct_builder::Config, constructed with the most efficient
   * yet safe values, for transport::struc::Msg_out (out-messages) compatible with #Structured_channel upgraded-from
   * #Channel_obj channels opened via this Session_mv type.  Informally, typically, this is useful if and only if you'd
   * like to construct a transport::struc::Msg_out, and:
   *   - you lack a struc::Channel object from which to issue struc::Channel::create_msg() or
   *     struc::Channel::struct_builder_config(), or if you have one, but the target out-message is
   *     intended for multiple channels, so you don't want to stylistically tie the out-message's creation
   *     to any particular one channel; and
   *   - you lack a Session_mv object from which to issue non-static `this->heap_fixed_builder_config()`, or if
   *     you have one, but the target out-message intended for multiple sessions, so you don't want
   *     to stylistically tie the out-message's creation to any particular one Session_mv.
   *
   * Otherwise one would typically use `struc::Channel::create_msg()/struct_builder_config()` or a
   * `this->heap_fixed_builder_config()` respectively.
   *
   * ### Usability in SHM-based Session impls ###
   * Do note this method is available, via base class, in SHM-based Session impls
   * (e.g., shm::classic::Server_session and shm::classic::Client_session) -- nothing forbids the use of
   * heap-based messaging even in such a session.  However it would typically be better for performance
   * (and in many cases is the reason for using a SHM-based Session impl in the first place) to use the
   * non-heap-based counterparts to this method.  E.g., see
   * shm::classic::Session_mv::session_shm_builder_config() and
   * shm::classic::Session_mv::app_shm_builder_config(); possibly
   * shm::classic::Session_server::app_shm_builder_config().
   *
   * @param logger_ptr
   *        Logger to pass to the returned `Config`.
   * @return See above.
   */
  static transport::struc::Heap_fixed_builder::Config heap_fixed_builder_config(flow::log::Logger* logger_ptr);

  /**
   * Utility that obtains a heap-based (non-zero-copy) Struct_builder::Config, constructed with the most efficient
   * yet safe values, for transport::struc::Msg_out (out-messages) compatible with #Structured_channel upgraded-from
   * #Channel_obj channels opened via `*this` Session_mv.  It is, simply, `heap_fixed_builder_config(L)`, where
   * `L` is the Flow `Logger*` belonging to `*this` Session.
   *
   * Informally, typically, this is useful if and only if you'd like to construct a transport::struc::Msg_out, and:
   *   - you lack a struc::Channel object from which to issue struc::Channel::create_msg() or
   *     struc::Channel::struct_builder_config(), or if you have one, but the target out-message is
   *     intended for multiple channels, so you don't want to stylistically tie the out-message's creation
   *     to any particular one channel; and
   *   - you do have a Session_mv object from which to issue non-static `this->heap_fixed_builder_config()`.
   *
   * ### Usability in SHM-based Session impls ###
   * The note in the other overload's doc header applies equally.
   *
   * @return See above.
   */
  transport::struc::Heap_fixed_builder::Config heap_fixed_builder_config();

  /**
   * Deserializing counterpart to `static` heap_fixed_builder_config().  It is mostly provided for consistency,
   * as it simply returns `Heap_reader::Config(logger_ptr)`.
   *
   * @param logger_ptr
   *        Logger to pass to the returned `Config`.
   * @return See above.
   */
  static transport::struc::Heap_reader::Config heap_reader_config(flow::log::Logger* logger_ptr);

  /**
   * Deserializing counterpart to non-`static` heap_fixed_builder_config().  It is mostly provided for consistency,
   * as it simply returns `Heap_reader::Config(L)`, where `L` is the Flow `Logger*` belonging to `*this` Session.
   *
   * @return See above.
   */
  transport::struc::Heap_reader::Config heap_reader_config();

  /**
   * Returns logger (possibly null).
   * @return See above.
   */
  flow::log::Logger* get_logger() const;

  /**
   * Returns log component.
   * @return See above.
   */
  const flow::log::Component& get_log_component() const;

protected:
  // Types.

  /// Short-hand for #Impl's Session_base super-class.
  using Session_base_obj = typename Impl::Session_base_obj;

  /// Short-hand for `const`-respecting wrapper around #Impl for the pImpl idiom.  See impl().
  using Impl_ptr = std::experimental::propagate_const<boost::movelib::unique_ptr<Impl>>;

  // Methods.

  /**
   * Provides `const` access to Session_base super-object.
   * @return See above.
   */
  const Session_base_obj& base() const;

  /**
   * pImpl target; particularly for sub-classes that must add to the above `public` API.
   * @return See above.  May be null; namely if `*this` is as-if default-cted.
   */
  Impl_ptr& impl();

  /**
   * pImpl target; particularly for sub-classes that must add to the above `public` API.
   * @return See above.  May be null; namely if `*this` is as-if default-cted.
   */
  const Impl_ptr& impl() const;

private:
  // Friends.

  /// Friend of Session_mv.
  template<typename Session_impl_t2>
  friend std::ostream& operator<<(std::ostream& os,
                                  const Session_mv<Session_impl_t2>& val);

  // Data.

  /// The true implementation of this class.  See also our class doc header.
  Impl_ptr m_impl;
}; // class Session_mv

// Free functions: in *_fwd.hpp.

// Template implementations (strict pImpl-idiom style (albeit pImpl-lite due to template-ness)).

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SESSION_MV \
  template<typename Session_impl_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SESSION_MV \
  Session_mv<Session_impl_t>

TEMPLATE_SESSION_MV
typename CLASS_SESSION_MV::Impl_ptr& CLASS_SESSION_MV::impl()
{
  return m_impl;
}

TEMPLATE_SESSION_MV
const typename CLASS_SESSION_MV::Impl_ptr& CLASS_SESSION_MV::impl() const
{
  return m_impl;
}

// The performant move semantics we get delightfully free with pImpl; they'll just move-to/from the unique_ptr m_impl.

TEMPLATE_SESSION_MV
CLASS_SESSION_MV::Session_mv(Session_mv&&) = default;

TEMPLATE_SESSION_MV
CLASS_SESSION_MV& CLASS_SESSION_MV::operator=(Session_mv&&) = default;

// The default ctor comports with how null m_impl is treated all over below.
TEMPLATE_SESSION_MV
CLASS_SESSION_MV::Session_mv() = default;

// The rest is strict forwarding to impl() (when not null).

// It's only explicitly defined to formally document it.
TEMPLATE_SESSION_MV
CLASS_SESSION_MV::~Session_mv() = default;

TEMPLATE_SESSION_MV
const Session_token& CLASS_SESSION_MV::session_token() const
{
  return impl() ? impl()->session_token() : transport::struc::NULL_SESSION_TOKEN;
}

TEMPLATE_SESSION_MV
typename CLASS_SESSION_MV::Mdt_builder_ptr CLASS_SESSION_MV::mdt_builder()
{
  return impl() ? impl()->mdt_builder() : Mdt_builder_ptr();
}

TEMPLATE_SESSION_MV
bool CLASS_SESSION_MV::open_channel(Channel_obj* target_channel, const Mdt_builder_ptr& mdt,
                                    Error_code* err_code)
{
  return impl() ? impl()->open_channel(target_channel, mdt, err_code) : false;
}

TEMPLATE_SESSION_MV
bool CLASS_SESSION_MV::open_channel(Channel_obj* target_channel, Error_code* err_code)
{
  return impl() ? impl()->open_channel(target_channel, err_code) : false;
}

TEMPLATE_SESSION_MV
transport::struc::Heap_fixed_builder::Config
  CLASS_SESSION_MV::heap_fixed_builder_config(flow::log::Logger* logger_ptr) // Static.
{
  return Impl::heap_fixed_builder_config(logger_ptr);
}

TEMPLATE_SESSION_MV
transport::struc::Heap_fixed_builder::Config
  CLASS_SESSION_MV::heap_fixed_builder_config()
{
  return Impl::heap_fixed_builder_config(impl() ? impl()->get_logger() : nullptr);
}

TEMPLATE_SESSION_MV
transport::struc::Heap_reader::Config
  CLASS_SESSION_MV::heap_reader_config(flow::log::Logger* logger_ptr) // Static.
{
  return Impl::heap_reader_config(logger_ptr);
}

TEMPLATE_SESSION_MV
transport::struc::Heap_reader::Config
  CLASS_SESSION_MV::heap_reader_config()
{
  return Impl::heap_reader_config(impl() ? impl()->get_logger() : nullptr);
}

TEMPLATE_SESSION_MV
flow::log::Logger* CLASS_SESSION_MV::get_logger() const
{
  return impl()->get_logger();
}

TEMPLATE_SESSION_MV
const flow::log::Component& CLASS_SESSION_MV::get_log_component() const
{
  return impl()->get_log_component();
}

TEMPLATE_SESSION_MV
const typename CLASS_SESSION_MV::Session_base_obj& CLASS_SESSION_MV::base() const
{
  assert(impl() && "Session default ctor is meant only for being moved-to.  Internal bug?");
  return impl()->base();
}

TEMPLATE_SESSION_MV
std::ostream& operator<<(std::ostream& os, const CLASS_SESSION_MV& val)
{
  if (val.impl())
  {
    return os << *val.impl();
  }
  // else
  return os << "null";
}

#undef CLASS_SESSION_MV
#undef TEMPLATE_SESSION_MV

} // namespace ipc::session
