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
#include <boost/thread/future.hpp>

namespace ipc::session
{

// Types.

/**
 * Internal type containing data and types common to internal types Server_session_impl and Client_session_impl
 * which are the respective true cores of #Server_session and #Client_session respectively.
 *
 * A Server_session_impl and Client_session_impl share many basic properties, some public.  For example consider
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
 * Session_base also has a `protected` inner class, Session_base::Graceful_finisher which certain variations
 * (for which this is necessary) of Server_session_impl and Client_session_impl use to carry out a
 * graceful-session-end handshake procedure.  This is here in Session_base -- despite being somewhat more
 * algorithmic/stateful than the other stuff here -- essentially because the Graceful_finisher (if needed at
 * all) on each side acts completely symmetrically.  Other items tend to be (internally) asymmetrical in
 * behavior between Server_session_impl and Client_session_impl (and variants).  More info on
 * Session_base::Graceful_finisher in its own doc header.
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
   * This value shall be known and immutable from construction for Server_session_impl, since server namespace -- PID
   * as of this writing -- can be determined from the start on the server side and applies to every server session
   * that Session_server produces.  Client_session_impl, however, determines
   * it at the latest possible moment which is at Client_session_impl::async_connect() time, at which point it needs
   * to determine the PID via PID file.
   *
   * @return See above.
   */
  const Shared_name& srv_namespace() const;

  /**
   * See Server_session_impl, Client_session_impl.  However in Session_base this value may be not-yet-set (empty), or
   * set (and immutable from then on).
   *
   * This value shall be generated uniquely (within some context) for each new `Server_session` produced by
   * Session_server; and Client_session_impl shall find that out while logging in (last part of entering PEER
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
   * This value shall be set from the start in a Client_session_impl but determined during a given Server_session_impl's
   * log-in (the opposing Client_session_impl will transmit the Client_app::m_name).  The log-in shall complete the
   * `Server_session`'s entry to PEER state.
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
   * to which client shall transport::Native_socket_stream::sync_connect() in order to establish a PEER-state
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
   * Reference to Server_app (referring to local process in Server_session_impl, opposing process in
   * Client_session_impl).
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
   * calling Server_session_impl::init_handlers(), or calling it late, as of this writing can cause issues with this.
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

  /**
   * Optional to use by subclasses, this operates a simple state machine that carries out a graceful-session-end
   * handshake procedure.  A particular Client_session_impl and symmetrically its opposing Server_session_impl
   * shall instantiate a `*this` internally and invoke its methods on certain events, as described in their
   * contracts.
   *
   * Now we explain what we're solving (Rationale); then how we solve it (Design).  The latter is much simpler,
   * we think, to understand then the former.  Hence apologies in advance for the length of "Rationale" section.
   *
   * ### Rationale ###
   * Consider a particular Client_session_impl + Server_session_impl class/object pair, A and B.  In this case --
   * which is somewhat unusual (relative to most internal `Session` algorithms) but in a good, simplifying way --
   * it does not matter which is A and which is B.  So let's say A is us and B is the opposing guy.
   *
   * Also, for now, assume these are just the vanilla ones: session::Client_session_impl and
   * session::Server_session_impl; not SHM-enabled -- no SHM arenas in play.
   *
   * Consider what happens when A dtor is invoked, in PEER state.  The session ends, and the session-end trigger is us.
   * Without Graceful_finisher what happens is as follows.  The #Master_structured_channel in `*this` is closed;
   * and then `*this` goes away altogether right away/synchronously.  Soon B's #Master_structured_channel
   * throws the graceful-close error; B itself correctly interprets this as a session-end trigger; and B throws
   * an error via the `Session` error handler.  Now, the user is told by docs they informally *should* destroy
   * the B (invoke its dtor) ASAP, but nothing is formally forcing them to; it can sit around.  It will never
   * accept channel passive-opens; `open_channel()` will no-op / return null; etc.  It is useless, but it can stay
   * around.  Eventually B dtor will be invoked by user; it'll basically clean up data structures, and that's that.
   *
   * There is no problem there: A dtor and B dtor can run at quite different times, or more precisely B dtor
   * can run much later than A dtor which triggered session-end.  Nothing bad happens.
   *
   * Now we'll discuss this in specific terms around actual `Client/Server_session_impl` variants that do exist
   * at this time.  We could try to be formal and general, but it's easier to just be specific for exposition
   * purposes.
   *
   * Take, now, the shm::classic::Client_session_impl shm::classic::Server_session_impl variants.  The above
   * generally holds, but there are now SHM arenas in play.  We can forget about the app-scope arenas right off
   * the bat: That guy stays around as long as the `Session_server` does, and that guy is formally required
   * to be around until all child `Session`s have been destroyed.  So only session-scope arenas -- the ones
   * inside A and B -- are relevant.  In fact, for SHM-classic there really is only *one* arena: A holds a
   * handle to it (`a.session_shm()` points to it), and B does so symmetrically.
   *
   * Now reconsider the scenario above: A dtor runs.  (Insert vanilla `*_session_impl` text from above here.)
   * Also, though, A dtor will remove the underlying SHM-pool by its name.  (This is possibly a lie.  This is
   * technically so, only if A is a `Server_session_impl`; we've assigned the task to the server side as of this
   * writing.  But this technicality is irrelevant: unlinking the SHM-pool doesn't blow up the other side's
   * access to the arena; it remains in RAM until that guy closes.  So just bear with us here.)  Then indeed
   * B dtor can run sometime later.  Is there a problem?  For SHM-classic, which is what we're discussing, no:
   * The arena -- 1 shared SHM-pool in this case -- stays alive, until B's handle gets closed by B dtor.
   * What about allocations/deallocations?  No problem there either: By contract, the A user *must* drop all
   * cross-process `shared_ptr` handles to objects that it owns, before invoking A dtor; so A is fine, and the
   * internal cross-process ref-counts for each A-held object get decremented by 1, due to the `shared_ptr` handles
   * being nullified by the user by contract.  (For context: The guided manual `session_app_org` page describes
   * to the reader how to structure their program, so that this is guaranteed.  In short, declare the object
   * handle data members in their code after the `Session` data member, so they get deinitialized in the reverse
   * order, in the user's hypothetical recommended `App_session` class.)  Isn't there a problem in B, though?  No:
   * the B user shall drop *its* `shared_ptr` handles, before invoking B dtor; this will drop their internal
   * cross-process ref-counts to 0; at which point shm::Pool_arena logic will deallocate those objects.  Deallocation
   * is entirely distributed between A and B; in this scenario B's handles just happen to live longer and therefore
   * happen to be the ones triggering the deallocations -- by the B process itself, synchronously.
   * A is not "in charge" of deallocations of anything in any special way; it doesn't matter which process originally
   * allocated a given object either.  That's the beauty/simplicity of SHM-classic.  (There are of course trade-offs
   * in play; this is worse for segregation/safety... see Manual and such.)
   *
   * Now we come to the point.  Consider session::shm::arena_lend::jemalloc::Client_session_impl and
   * `Server_session_impl`.  There *is* a problem.  Interestingly this time it really does *not* matter whether
   * A is server or client-side.  In SHM-jemalloc the session-scope arenas are not 1 but 2:
   * A has an arena, from which process A can allocate, and in which process A shall deallocate.
   * B has an arena, from which process B can allocate, and in which process B shall deallocate.
   * If B borrows object X from A, and is later done with it -- the `shared_ptr` local ref-count reached 0 --
   * SHM-jemalloc-arranged `shared_ptr` deleter will send an internal message over a special channel to A.
   * A will deallocate X once that has occurred, *and* any A-held `shared_ptr` group for X has also reached
   * ref-count 0 (which may have happened before, or it may happen later).  Exactly the same is true of A borrowing
   * from B, conversely/symmetrically.
   *
   * The problem is this: Suppose B holds a borrowed handle X (to an A-allocated object).  Now A dtor runs;
   * its `.session_shm()` arena -- its constitutent SHM-pools! -- is destroyed.  That in itself shouldn't be
   * a problem; again B presumably holds a SHM-pool handle, so it won't just disappear from RAM under it.
   * I (ygoldfel) am somewhat fuzzy on what exactly happens (not being the direct author of SHM-jemalloc), but
   * basically as I understand it, in destroying the arena, a bunch of jemalloc deinit steps execute; perhaps
   * heap-marker data structures are modified all over the place... it's chaos.  The bottom line is:
   * A arena gets mangled, so the B-held borrowed handle X has a high chance of pointing, now, to garbage:
   * Not unmapped garbage; not into a SHM-pool that's magically gone -- it's still there -- so there's not necessarily
   * a SEGV as a result; just garbage in the ~sense of accessing `free()`d (not un-`mmap()`ped) memory.
   *
   * All that is to say: A dtor runs; and user handle to object X in B instantly becomes unusable.
   * For example, I (ygoldfel) have observed a simple thing: A SHM-jemalloc-backed struc::Msg_in received from
   * A, in B, was absolutely fine.  Then it was running a long tight-loop verification of its contents -- verifying
   * hashes of millions of X-pointed objects in SHM, preventing B dtor from running.  Meanwhile, A in a test program
   * had nothing more to do and ran A dtor (closed session).  Suddenly, the hash-verifier was hitting hash
   * mismatches, throwing capnp exceptions in trying to traverse the message, and so on.
   *
   * What this means, ultimately is straightforward: A dtor must not destroy its SHM-jemalloc arena
   * (as accessible normally through `.session_shm()`), until not just *A* user has dropped all its object
   * `shared_ptr` handles; but *B* user -- that's in the other process, B -- has as well!  How can we enforce
   * this, though?  One approach is to just put it on the user: Tell them that they must design their protocol
   * in such a way as to arrange some kind of handshake, as appropriate, beyond which each side knows for a fact
   * that the session objects, and therefore arenas, are safe to destroy.
   *
   * We don't want to do that for obvious reasons.  What we already tell them should be sufficient: If you get
   * a session-hosing error handler execution in your `Session`, then don't use that `Session`, and you should
   * destroy it ASAP, for it is useless, and the other side's `Session` is not long for this life.  If they
   * listen to this, then A dtor will soon be followed by B dtor anyway.  It might be delayed by some long
   * tight-loop operation such as the test-program hash-verifying scenario above, but generally users avoid such
   * things; and when they don't they can expect other things to get blocked, possibly in the opposing process.
   * Subjectively, weird blocking -- caused by the other side acting pathologically -- is a lot more acceptable
   * at session-start or session-end than something like that occuring mid-session.
   *
   * So what's the exact proposal?  It's this: If A dtor begins executing, it must first block until it knows
   * B dtor has begun executing; but once it does know that, it knows that all B-held object handles have been
   * dropped -- which is what we want.  (It would be possible for B user to indicate it some other way -- another
   * API call, short of destruction -- but that seems unnecessarily complex/precious.  If the session is finished,
   * then there's no reason to keep the `Session` object around = the overall policy we've pursued so far, so why
   * make exceptions for this?)
   *
   * To be clear, this means certain behavior by B user can cause A dtor to block.  We can explain this in the docs,
   * of course, but perhaps much more effective is to make it clear in the logs: say the wait begins here and what
   * it'll take for it to end; and say when it has ended and why.
   *
   * Also to be clear, Graceful_finisher should be used only in `*_session_impl` pairs that actually have this
   * problem; so of the 3 pairs we've discussed above -- only SHM-jemalloc's ones.  The others can completely
   * forego all this.
   *
   * ### Design / how to use ###
   * How to use: Subclass shall create a `*this` if and only if the feature is required; it shall call its methods
   * and ctor on certain events, as prescribed in their doc headers.
   *
   * Design: All this assumes PEER state.  Now:
   *
   * Again let's say we're A, and they're B -- and just like we are.  Our goal is to ensure A dtor, at its
   * start, blocks until B dtor has started executing; and to give their side the information necessary to do the
   * same.  That's on_dtor_start(): the method that'll do the blocking; when it returns the dtor of subclass can
   * proceed.  So what's needed for on_dtor_start() to `return`?  One, A dtor has to have started -- but that's true
   * by the contract of when the method is called; two, B dtor has to have started.  How do we get informed of the
   * latter?  Answer: B sends a special message along the #Master_structured_channel, from its own on_dtor_start().
   * Naturally, we do the same -- in full-duplex fashion in a sense -- so that they can use the same logic.  So then
   * the steps in on_dtor_start() are:
   *   -# Send `GracefulSessionEnd` message to B, if possible (#Master_structured_channel is up, and the send
   *      op doesn't return failure).
   *   -# Perform `m_opposing_session_done.get_future().wait()`, where `m_opposing_session_done` is a `promise<void>`
   *      that shall be fulfilled on these events (and only if it hasn't already been fulfilled before):
   *      - The #Master_structured_channel has received `GracefulSessionEnd` *from* B.
   *      - The #Master_structured_channel has emitted a channel-hosing error.
   *
   * Notes:
   *   - The mainstream case is receiving `GracefulSessionEnd`: In normal operation, with no acts of god, we will no
   *     longer destroy the session-master-channel, until returning from on_dtor_start().
   *     - This could happen after the `.wait()` starts -- meaning A dtor indeed ran before B dtor (our side is
   *       the one triggering session-end).
   *     - It could just as easily happen *before* the `.wait()` starts -- meaning the reverse.  Then `.wait()`
   *       will return immediately.
   *   - Of course who knows what's going on -- the #Master_structured_channel could still go down without
   *     the `GracefulSessionEnd`.  Is it really right to treat this the same, and let on_dtor_start() return?
   *     Answer: Yes.  Firstly, there's hardly any other choice: the channel's dead; there's no other way of knowing
   *     what to wait for; and we can't just sit there cluelessly.  But more to the point, this means it's a
   *     *not*-graceful scenario: Graceful_finisher can't hope to deal with it and can only proceed heuristically.
   *     This is not networking; so we should have a pretty good level of predictability at any rate.
   *
   * Last but not least consider that by delaying the destruction of the #Master_structured_channel until after
   * the dtor has started (assuming no channel-hosing acts of god), we've changed something: The error handler shall
   * no longer inform the user of a session-end trigger from the other side!  Left alone this way, we've broken
   * the whole system by introducing a chicken-egg paradox: We don't let our user be informed of the session-end
   * trigger when would normally, so they won't call our dtor; but because they won't call our dtor, we'll never
   * reach the point where the channel would get hosed and we'd know the inform the user.  How to fix this?
   * The answer is pretty obvious: Receiving the `GracefulSessionEnd` shall now trigger a special graceful-session-end
   * error.  Naturally this would only be usefully emitted if we're not in the dtor yet.
   * So the latter situation should kick-off the user invoking our dtor sometime soon, hopefully; we'll send our
   * `GracefulSessionEnd` to them; and so on.
   */
  class Graceful_finisher :
    public flow::log::Log_context,
    private boost::noncopyable
  {
  public:
    // Constructors/destructor.

    /**
     * You must invoke this ctor (instantiate us) if and only if synchronized dtor execution is indeed required;
     * and `*this_session` has just reached PEER state.  Invoke from thread W only.
     *
     * ### How `master_channel` shall be touched by `*this` ###
     * We're being very explicit about this, since there's some inter-class private-state sharing going on...
     * generally not the most stable or stylistically-amazing situation....
     *   - This ctor will `.expect_msg(GRACEFUL_SESSION_END)`, so we can detect its arrival and mark it down as needed
     *     and possibly invoke `this_session->hose()` to report it to the user.
     *     Hence there's no method you'll need to call nor any setup like this needed on your part.
     *   - on_dtor_start() will attempt to send `GracefulSessionEnd` to the opposing Graceful_finisher.
     *
     * @param logger_ptr
     *        Logger to use for logging subsequently.  (Maybe `Session_base` should just subclass `Log_context`
     *        for all os us?  Add to-do?)
     * @param this_session
     *        The containing Session_base.
     * @param async_worker
     *        The thread W of the containing `*_session_impl`.  `GracefulSessionEnd` handler is at least partially
     *        invoked there (`hose(...)`); and on_dtor_start() posts onto it.
     * @param master_channel
     *        The containing `Session` master channel.  The pointee must exist until `*this` Graceful_finisher
     *        is gone.
     */
    explicit Graceful_finisher(flow::log::Logger* logger_ptr, Session_base* this_session,
                               flow::async::Single_thread_task_loop* async_worker,
                               Master_structured_channel* master_channel);

    // Methods.

    /**
     * Must be invoked if the `*_session_impl` detects that the master channel has emitted channel-hosing error.
     *
     * Invoke from thread W.
     */
    void on_master_channel_hosed();

    /**
     * The reason Graceful_finisher exists, this method must be called at the start of `*_session_impl` dtor; and
     * will block until it is appropriate to let that dtor proceed to shut down the `*_session_impl`.
     *
     * Invoke from thread U, not thread W.
     */
    void on_dtor_start();

  private:
    // Data.

    /// The containing Session_base.  It shall exist until `*this` is gone.
    Session_base* const m_this_session;

    /// The containing `Session` thread W loop.  It shall exist until `*this` is gone.
    flow::async::Single_thread_task_loop* m_async_worker;

    /// The containing `Session` master channel.  It shall exist until `*this` is gone.
    Master_structured_channel* m_master_channel;

    /**
     * A promise whose fulfillment is a necessary and sufficient condition for on_dtor_start() returning
     * (letting `Session` dtor complete).
     *
     * It is fulfilled once the #Master_structured_channel receives `GracefulSessionEnd` from opposing side
     * (indicating the opposing on_dtor_start() has started) or got hosed (indicating we'll now never know this
     * and must act as-if opposing on_dtor_start() has started).  See "Design" in our doc header.
     */
    boost::promise<void> m_opposing_session_done;
  }; // class Graceful_finisher

  // Constructors.

  /**
   * Constructs: Client_session_impl form (the user is the one constructing the object, though in NULL state).
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
   * Constructs: Server_session_impl form (Session_server is the one constructing the object, though in NULL state,
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
   * Returns `true` if and only if set_on_err_func() has been called.
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
   *     As of this writing this is Server_session_impl's or Client_session_impl's thread W in practice, which is how
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
   * in Client_session_impl or Server_session_impl.  In both cases these `ostream<<`s access only #m_srv_app_ref, which
   * is immutable throughout (so no problem there) and #m_cli_app_ptr.  For Client_session_impl #m_cli_app_ptr is
   * also immutable throughout (so no problem there).  For Server_session_impl however it does change in one spot:
   * Server_session_impl::async_accept_log_in() internal async handler for the log-in request shall, on success,
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
  /* This is, basically, the (protected) ctor for Client_session, followed by Client_session_impl::async_connect().
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
CLASS_SESSION_BASE::Graceful_finisher::Graceful_finisher(flow::log::Logger* logger_ptr, Session_base* this_session,
                                                         flow::async::Single_thread_task_loop* async_worker,
                                                         Master_structured_channel* master_channel) :
  flow::log::Log_context(logger_ptr, Log_component::S_SESSION),
  m_this_session(this_session),
  m_async_worker(async_worker),
  m_master_channel(master_channel)
{
  // We are in thread W.

  m_master_channel->expect_msg(Master_structured_channel::Msg_which_in::GRACEFUL_SESSION_END,
                               [this](auto&&)
                               // (The message doesn't matter and contains no fields; only that we received it matters.)
  {
    // We are in thread Wc (unspecified, really struc::Channel async callback thread).
    m_async_worker.post([this]()
    {
      // We are in thread W.  (We need to be to safely trigger hosed() and hose().)

      FLOW_LOG_INFO("Received GracefulSessionEnd from opposing Session object along session master channel "
                    "[" << *m_master_channel << "].  Will emit to local user as graceful-end "
                    "error; and mark it down.  It our Session dtor is running, it shall return soon.  It it has "
                    "not yet run, it shall return immediately once it does execute.");

      m_opposing_session_done.set_value();
      /* Can m_opposing_session_done already be set?  (That would throw promise exception, and we don't catch it
       * above.)  Answer: no, in the current protocol at least: Only once can it be sent (hence .expect_msg(), not
       * .expect_msgs()); and if *m_master_channel were to be hosed, then that would occur either after
       * expect_msg() firing handler, or it would occur instead of it.  (We *do* handle the "after" case in
       * on_master_channel_hosed(), where we do indeed catch the exception.) */

      if (!m_this_session->hosed())
      {
        m_this_session->hose(error::Code::S_SESSION_FINISHED); // It'll log.
      }
    }); // m_async_worker.post()
  }); // m_master_channel->expect_msg(GRACEFUL_SESSION_END)
} // Session_base::Graceful_finisher::Graceful_finisher()

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::Graceful_finisher::on_master_channel_hosed()
{
  using boost::promise_already_satisfied;

  // We are in thread W.
  try
  {
    m_opposing_session_done.set_value();
  }
  catch (const promise_already_satisfied&)
  {
    // Interesting.  @todo Maybe log?
  }
} // } // Session_base::Graceful_finisher::on_master_channel_hosed()

TEMPLATE_SESSION_BASE
void CLASS_SESSION_BASE::Graceful_finisher::on_dtor_start()
{
  using flow::async::Synchronicity;

  // We are in thread U.  In fact in we're in a Client/Server_session_impl dtor... but at its very start.

  /* Thread W must be fine and running; per algorithm (see Graceful_finisher class doc header).
   * It's only safe to access m_master_channel from there. */
  m_async_worker->post([&]()
  {
    FLOW_LOG_INFO("In Session object dtor sending GracefulSessionEnd to opposing Session object along "
                  "session master channel [" << *m_master_channel << "] -- if at all possible.");

    auto msg = m_master_channel->create_msg();
    msg.body_root()->initGracefulSessionEnd();

    Error_code err_code_ignored;
    m_master_channel->send(msg, nullptr, &err_code_ignored);
    /* Whatever happened, it logged.  We make a best effort; it channel is hosed or send fails, then the other
     * side shall detect it via its on_master_channel_hosed() presumably and set its m_opposing_session_done, hence
     * its on_dtor_start() will proceed. */
  }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); // m_async_worker->post()

  // That would've been non-blocking.  So now lastly:

  FLOW_LOG_INFO("In Session object dtor we not await sign that the opposing Session object dtor has also started; "
                "only then will we proceed with destroying our Session object: e.g., maybe they hold handles "
                "to objects in a SHM-arena we would deinitialize.  If their dtor has already been called, we will "
                "proceed immediately.  If not, we will now wait for that.  This would only block if the opposing "
                "side's user code neglects to destroy Session object on error; or has some kind of blocking "
                "operation in progress before destroying Session object.  "
                "Session master channel: [" << *m_master_channel << "].");
  m_opposing_session_done.get_future().wait();
  FLOW_LOG_INFO("In Session object dtor: Done awaiting sign that the opposing Session object dtor has also started.  "
                "Will now proceed with our Session object destruction.");
} // Session_base::Graceful_finisher::on_dtor_start()

TEMPLATE_SESSION_BASE
typename CLASS_SESSION_BASE::Structured_msg_reader_config
  CLASS_SESSION_BASE::heap_reader_config(flow::log::Logger* logger_ptr) // Static.
{
  return transport::struc::Heap_reader::Config{ logger_ptr, 0 };
} // Session_base::heap_reader_config()

#undef CLASS_SESSION_BASE
#undef TEMPLATE_SESSION_BASE

} // namespace ipc::session
