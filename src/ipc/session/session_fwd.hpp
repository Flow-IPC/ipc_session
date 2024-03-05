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
#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/native_handle.hpp"

/**
 * Flow-IPC module providing the broad lifecycle and shared-resource organization -- via the *session* concept --
 * in such a way as to make it possible for a given pair of processes A and B to set up ipc::transport
 * structured- or unstructured-message channels for general IPC, as well as to share data in
 * SHared Memory (SHM).  See namespace ::ipc doc header for an overview of Flow-IPC modules
 * including how ipc::session relates to the others.  Then return here.  A synopsis follows:
 *
 * It is possible to use the structured layer of ipc::transport, namely the big daddy transport::struc::Channel,
 * without any help from ipc::session.  (It's also possible to establish unstructured transport::Channel and
 * the various lower-level IPC pipes it might comprise.)  And, indeed,
 * once a given `struc::Channel` (or `Channel`) is "up," one can and *should*
 * simply use it to send/receive stuff.  The problem that ipc::session solves is in establishing
 * the infrastructure that makes it simple to (1) open new `struc::Channel`s or `Channel`s or SHM areas;
 * and (2) properly deal with process lifecycle events such as the starting and ending (gracefully or otherwise) of
 * the local and partner process.
 *
 * Regarding (1), in particular (just to give a taste of what one means):
 *   - What underlying low-level transports will we even be using?  MQs?  Local (Unix domain) stream sockets?
 *     Both?
 *   - For each of those, we must connect or otherwise establish each one.  In the case of MQs, say, there has
 *     to be an agreed-upon util::Shared_name for the MQ in each direction... so what is that name?
 *     How to prevent collisions in this name?
 *
 * While ipc::transport lets one do whatever one wants, with great flexibility, ipc::session establishes conventions
 * for all these things -- typically hiding/encapsulating them away.
 *
 * Regarding (2) (again, just giving a taste):
 *   - To what process are we even talking?  What if we want to talk to 2, or 3, processes?  What if we want to talk
 *     to 1+ processes of application X and 1+ processes of application Y?  How might we segregate the data
 *     between these?
 *   - What happens if 1 of 5 instances of application X, to whom we're speaking, goes down?  How to ensure cleanup
 *     of any kernel-persistence resources, such as the potentially used POSIX MQs, or even of SHM (ipc::shm)?
 *
 * Again -- ipc::session establishes conventions for these lifecycle matters and provides key utilities such as
 * kernel-persistent resource cleanup.
 *
 * ### Basic concepts ###
 * An *application* is, at least roughly speaking, 1-1 with a distinct executable presumably interested in
 * communicating with another such executable.  A *process* is a/k/a *instance* of an application that has begun
 * execution at some point.  In the IPC *universe* that uses ::ipc, on a given machine, there is some number
 * of distinct applications.  If two processes A and B want to engage in IPC, then their apps A and B comprise
 * a meta-application *split*, in that (in a sense) they are one meta-application that have been split into two.
 *
 * A process that is actively operating, at least in the IPC sense, is called an *active* process.  A zombie
 * process in particular is not active, nor is one that has not yet initialized IPC or has shut it down, typically
 * during graceful (or otherwise) termination.
 *
 * In the ipc::session model, any two processes A-B must establish a *session* before engaging in IPC.  This
 * *session* comprises all *shared resources* pertaining to those two *processes* engaging in IPC.  (Spoiler alert:
 * at a high level these resources comprise, at least, *channels* of communication -- see transport::Channel --
 * between them; and may also optionally comprise SHared Memory (SHM).)  The session ends when either A terminates
 * or B terminates (gracefully or otherwise), and no earlier.  The session begins at roughly the earliest time
 * when *both* A and B are active simultaneously.  (It is possible either process may run before IPC and after IPC,
 * but for purposes of our discussion we'll ignore such phases are irrelevant for simplicity of exposition.)
 * An important set of shared resources is therefore *per-session shared resources*.  (However, as we're about to
 * explain, it is not the only type.)
 *
 * In the ipc::session model, in a given A-B split, one must be chosen as the *client*, the other as the *server*;
 * let's by convention here assume they're always listed in server-client order.  The decision must be made which
 * one is which, at compile time.  The decision as to which is which is an important one, when using ipc::session
 * and IPC in that model.  While a complete discussion of making that decision is outside the scope here, the main
 * points are that in the A-B split (A the server, B the client):
 *   - For each *process* pair A-B, there are per-session shared resources, as described above, and as pertains
 *     to these per-session resources A is no different from B.  Once either terminates, these resources
 *     are not usable and shall be cleaned up as soon as possible to free RAM/etc.
 *   - There may be multiple (active) instances of B at a given time but only one (active) instance of A.
 *   - Accordingly, A is special in that it may maintain some *per-app shared resources* (cf. per-session ones)
 *     which persist even when a given B *process* terminates (thus that A-B session ends) and are available
 *     for access both by
 *     - other concurrently active instances of B; and
 *     - any future active instances of B.
 *
 * More specifically/practically:
 *   - *Channels* are between the 2 processes A-B in session A-B.  If B1 and B2 are instances of (client) app Bp,
 *     a channel cannot be established between B1 and B2; only A-B1 and A-B2.  So channels are always *per-session*.
 *   - A *SHM area* may exist in the *per-session* scope.  Hence A-B1 have a per-session SHM area; and A-B2 have
 *     a per-session SHM area.  When the session ends, that SHM area goes away.
 *   - A *SHM area* may exist in the *per-app* scope.  Hence A-B* have a per-app-B SHM area, which persists
 *     throughout the lifetime of (server) process A and remains available as B1, B2, ... start and stop.
 *     It disappears once A stops.  It appears when the first instance of B establishes a session with A
 *     (i.e., lazily).
 *
 * ### Brief illustration ###
 * Suppose A is a memory cache, while B is a cache reader of objects.  A starts;
 * B1 and B2 start; so sessions A-B1 and A-B2 now exist.  Now each of B1, B2 might establish channel(s) and then
 * use it/them to issue messages and receive messages back.  Now A<->B1 channel(s) and A<->B2 channel(s) are
 * established.  No B1<->B2 channel(s) can be established.  (Note: transport::Channel and other
 * ipc::transport IPC techniques are -- in and of themselves -- session-unaware.  So B1 could speak to B2.
 * They just cannot use ipc::session to do so.  ipc::session facilitates establishing channels and SHM
 * in an organized fashion, and that organization currently does not support channels between instances of
 * the same client application.  ipc::session is not the only option for doing so.)
 *
 * Suppose, in this illustration, that A is responsible both for acquiring network objects (on behalf of B*)
 * and memory-caching them.  Then B1 might say, via an A-B1 channel, "give me object X."  A does not have it
 * in cache yet, so it loads it from network, and saves it in the *per-app-B* SHM area and replies (via
 * same A-B1 channel) "here is object X: use this SHM handle X'."  B1 then accesses X' directly in SHM.
 * Say B2 also says "give me object X."  It is cached, so A sends it X' again.  Now, say A-B1 session ends,
 * because B1 stops.  All A-B1 channels go away -- they are per-session resources -- and any per-session-A-B1 SHM
 * area (which we haven't mentioned in this illustration) also goes away similarly.
 *
 * A-B2 continues.  B2 could request X again, and it would work.  Say B3 now starts and requests X.  Again,
 * the per-app-B SHM area persists; A would send X' to B3 as well.
 *
 * Now suppose A stops, due to a restart.  This ends A-B2 and A-B3 sessions; and the per-app-B SHM area goes
 * away too.  Thus the server process's lifetime encompasses all shared resources in the A-B split.
 * Processes B2 and B3 continue, but they must await for a new active A process (instance) to appear, so that
 * they can establish new A-B2 and A-B3 sessions.  In the meantime, it is IPC no-man's-land: there is
 * no active per-app-B shared resources (SHM in our illustration) and certainly no per-session-A-B* shared
 * resources either.  Once A-B2 and A-B3 (new) sessions are established, shared resources can begin to be
 * acquired again.
 *
 * This illustrates that:
 *   - A is the cache server and expected to service multiple cache clients Bs.  This is asymmetric.
 *   - A can maintain shared resources, especially SHM, that outlive any individual B process.  This is asymmetric.
 *   - A-B channels are relevant only to a given A-B* session.  (Such per-session SHM areas can also exist.)
 *     This is symmetric between A and a given B instance.
 *   - No shared resources survive beyond a given cache server A process.  A given client B* must be prepared
 *     to establish a new session with A when another active instance of A appears, before it can access IPC
 *     channels or SHM.  In that sense a given *session* is symmetric: it begins when both A and a B* are active,
 *     and it ends when either of the 2 stops.
 *
 * ### 2+ splits co-existing ###
 * The above discussion concerned a particular A-B split, in which by definition A is the server (only 1 active
 * instance at a time), while B is the client (0+ active instances at a time).
 *
 * Take A, in split A-B.  Suppose there is app Cp also.  An A-C split is also possible (as is A-D, A-E, etc.).
 * Everything written so far applies in natural fashion.  Do note, however, that the per-app scope applies to
 * a given (client) application, not all client applications.  So the per-app-B SHM area is independent of the
 * per-app-C SHM area (both maintained by A).  Once the 1st instance of B establishes a session, that SHM area
 * is lazily created.  Once the 1st instance of C does the same, that separate SHM area is lazily created.
 * So B1, B2, ... access one per-app SHM area, while C1, C2, ... access another.
 *
 * Now consider app K, in split K-A.  Here K is the server, A is the client.  This, also, is allowed, despite
 * the fact A is server in splits A-B, A-C.  However, there is a natural constraint on A: there can only be
 * one active process of it at a time, because it is the server in at least 1 split.  It can be a client to
 * server K; but there would only ever be 1 instance of it establishing sessions against K.
 *
 * Informally: This is not necessarily frowned upon.  After all the ability to have multiple concurrently active
 * processes of a given app is somewhat exotic in the first place.  That said, very vaguely speaking, it is more
 * flexible to design a given app to only be server in every split in which it participates.
 *
 * ### Overview of relevant APIs ###
 * With the above explained, here is how the ipc::session API operates over these various ideas.
 *
 * The simplest items are App (description of an application, which might a client or a server or both),
 * Client_app (description of an application in its role as a client), and Server_app (you get the idea).
 * See their doc headers, of course, but basically these are very simple data stores describing the basic
 * structure of the IPC universe.  Each application generally shall load the same values into its master
 * lists of these structures.  References to immutable Client_app and Server_app objects are passed into
 * actual APIs having to do with the Session concept, to establish who can establish sessions with whom and
 * how.  Basically they describe the allowed splits.
 *
 * As noted at the very top, the real goal is to open channels between processes and possibly to share data in
 * SHM between them and others.  To get there for a given process pair A-B, one must establish session A-B, as
 * described above.  Having chosen which one is A (server) and which is B (client), and loaded Client_app
 * and Server_app structures to that effect, it is time to manage the sessions.
 *
 * An established session is said to be a session in PEER state and is represented on each side by an object
 * that implements the Session concept (see its important doc header).  On the client side, this impl
 * is the class template #Client_session.  On the server side, this impl is the class template #Server_session.
 * Once each respective side has its PEER-state Session impl object, opening channels and operating SHM areas
 * on a per-session basis is well documented in the Session (concept) doc header and is fully symmetrical
 * in terms of the API.
 *
 * #Client_session does not begin in PEER state.  One constructs it in NULL state, then invokes
 * `Client_session::sync_connect()` to connect to the server process if any exists; once it fires its handler
 * successfully, the #Client_session is a Session in PEER state.  If #Client_session, per Session concept requirements,
 * indicates the session has finished (due to the other side ending session), one must create a new #Client_session
 * and start over (w/r/t IPC and relevant shared resources).
 *
 * Asymmetrically, on the server side, one must be ready to open potentially multiple `Server_session`s.
 * Firstly create a Session_server (see its doc header).  To open 1 #Server_session, default-construct one
 * (hence in NULL state), then pass it as the target to Session_server::async_accept().  Once it fires
 * your handler, you have an (almost-)PEER state Server_session, and moreover you may call
 * `Server_session::client_app()` to determine the identity of the connecting client app
 * (via Client_app) which should (in multi-split situations) help decide what to further do with this
 * Server_session (also, as on the opposing side, now a PEER-state Session concept impl).
 * (If A participates in splits A-B and A-C, then the types of IPC work it might do
 * with a B1 or B2 is likely to be quite different from same with a C1 or C2 or C3.  If only split A-B is relevant
 * to A, then that is not a concern.)
 *
 * As noted, in terms of per-session shared resources, most notably channel communication, a Server_session and
 * Client_session have identical APIs with identical capabilities, each implementing the PEER-state Session concept
 * to the letter.
 *
 * ### SHM ###
 * Extremely important (for performance at least) functionality is provided in the sub-namespace of ipc::session:
 * ipc::session::shm.  Please read its doc header now before using the types directly within ipc::session.
 * That will allow you to make an informed choice.
 *
 * ### `sync_io`: integrate with `poll()`-ish event loops ###
 * ipc::session APIs feature exactly the following asynchronous (blocking, background, not-non-blocking, long...)
 * operations:
 *   - ipc::session::Session on-error and (optionally) on-passive-channel-open handlers.
 *   - `ipc::session::Client_session::async_conenct()` (same for other, including SHM-aware, variants).
 *   - ipc::session::Session_server::async_accept() (same for SHM-aware variants).
 *
 * All APIs mentioned so far operation broadly in the async-I/O pattern: Each event in question is reported from
 * some unspecified background thread; it is up to the user to "post" the true handling onto their worker thread(s)
 * as desired.
 *
 * If one desires to be informed, instead, in a fashion friendly to old-school reactor loops -- centered on
 * `poll()`, `epoll_wait()`, or similar -- a `sync_io` alternate API is available.  It is no faster, but it may
 * be more convenient for some applications.
 *
 * To use the alternate `sync_io` interface: Look into session::sync_io and its 3 adapter templates:
 *   - ipc::session::sync_io::Server_session_adapter;
 *   - ipc::session::sync_io::Client_session_adapter;
 *   - ipc::session::sync_io::Session_server_adapter.
 *
 * Exactly the aforementioned async APIs are affected.  All other APIs are available without change.
 */
namespace ipc::session
{

// Types.

/// Convenience alias for the commonly used type util::Shared_name.
using Shared_name = util::Shared_name;

/// Convenience alias for the commonly used type transport::struc::Session_token.
using Session_token = transport::struc::Session_token;

// Find doc headers near the bodies of these compound types.

struct App;
struct Server_app;
struct Client_app;

template<typename Session_impl_t>
class Session_mv;

template<typename Server_session_impl_t>
class Server_session_mv;
template<typename Client_session_impl_t>
class Client_session_mv;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload = ::capnp::Void>
class Session_server;

/**
 * A vanilla `Server_session` with no optional capabilities.  See Server_session_mv (+doc header) for full API as
 * well as its doc header for possible alternatives that add optional capabilities (such as, at least, SHM
 * setup/access).  Opposing peer object: #Client_session.  Emitted by: Session_server.
 *
 * The following important template parameters are *knobs* that control the properties of the session;
 * the opposing #Client_session must use identical settings.
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Identical to #Client_session.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Identical to #Client_session.
 * @tparam Mdt_payload
 *         See Session concept.  In addition the same type may be used for `mdt_from_cli_or_null` (and srv->cli
 *         counterpart) in Session_server::async_accept().  (Recall that you can use a capnp-`union` internally
 *         for various purposes.)
 *
 * @see Server_session_mv for full API and its documentation.
 * @see Session: implemented concept.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload = ::capnp::Void>
using Server_session
  = Server_session_mv<Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>;

/**
 * A vanilla `Client_session` with no optional capabilities.  See Client_session_mv (+doc header) for full API as
 * well as its doc header for possible alternatives that add optional capabilities (such as, at least, SHM
 * setup/access).  Opposing peer object: #Server_session.
 *
 * The following important template parameters are *knobs* that control the properties of the session;
 * the opposing #Server_session must use identical settings.
 *
 * @tparam S_MQ_TYPE_OR_NONE
 *         Session::Channel_obj (channel openable via `open_channel()` on this or other side) type config:
 *         Enumeration constant that specifies which type of MQ to use (corresponding to all available
 *         transport::Persistent_mq_handle concept impls) or to not use one (`NONE`).  Note: This `enum` type is
 *         capnp-generated; see common.capnp for values and brief docs.
 * @tparam S_TRANSMIT_NATIVE_HANDLES
 *         Session::Channel_obj (channel openable via `open_channel()` on this or other side) type config:
 *         Whether it shall be possible to transmit a native handle via the channel.
 * @tparam Mdt_payload
 *         See Session concept.  In addition the same type may be used for `mdt` or `mdt_from_srv_or_null`
 *         in `*_connect()`.  (If it is used for `open_channel()` and/or passive-open and/or `*connect()`
 *         `mdt` and/or `mdt_from_srv_or_null`, recall that you can use a capnp-`union` internally for various
 *         purposes.)
 *
 * @see Client_session_mv for full API and its documentation.
 * @see Session: implemented concept.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES,
         typename Mdt_payload = ::capnp::Void>
using Client_session
  = Client_session_mv<Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>>;

// Free functions.

/**
 * Utility, used internally but exposed in public API in case it is of general use, that checks that the
 * owner of the given resource (at the supplied file system path) is
 * as specified in the given App (App::m_user_id et al).  If the resource cannot be accessed (not found,
 * permissions...) that system Error_code shall be emitted.  If it can, but the owner does not match,
 * error::Code::S_RESOURCE_OWNER_UNEXPECTED shall be emitted.
 *
 * @param logger_ptr
 *        Logger to use for logging (WARNING, on error only, including `S_RESOURCE_OWNER_UNEXPECTED`).
 * @param path
 *        Path to resource.  Symlinks are followed, and the target is the resource in question (not the symlink).
 * @param app
 *        Describes the app with the expected owner info prescribed therein.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        error::Code::S_RESOURCE_OWNER_UNEXPECTED (check did not pass),
 *        system error codes if ownership cannot be checked.
 */
void ensure_resource_owner_is_app(flow::log::Logger* logger_ptr, const fs::path& path, const App& app,
                                  Error_code* err_code = 0);

/**
 * Identical to the other ensure_resource_owner_is_app() overload but operates on a pre-opened `Native_handle`
 * (a/k/a handle, socket, file descriptor) to the resource in question.
 *
 * @param logger_ptr
 *        See other overload.
 * @param handle
 *        See above.  `handle.null() == true` causes undefined behavior (assertion may trip).
 *        Closed/invalid/etc. handle will yield civilized Error_code emission.
 * @param app
 *        See other overload.
 * @param err_code
 *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
 *        error::Code::S_RESOURCE_OWNER_UNEXPECTED (check did not pass),
 *        system error codes if ownership cannot be checked (invalid descriptor, un-opened descriptor, etc.).
 */
void ensure_resource_owner_is_app(flow::log::Logger* logger_ptr, util::Native_handle handle, const App& app,
                                  Error_code* err_code = 0);


/**
 * Prints string representation of the given `App` to the given `ostream`.
 *
 * @relatesalso App
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const App& val);

/**
 * Prints string representation of the given `Client_appp` to the given `ostream`.
 *
 * @relatesalso Client_app
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Client_app& val);

/**
 * Prints string representation of the given `Server_app` to the given `ostream`.
 *
 * @relatesalso Server_app
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Server_app& val);

/**
 * Prints string representation of the given `Session_mv` to the given `ostream`.
 *
 * @relatesalso Session_mv
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_impl_t>
std::ostream& operator<<(std::ostream& os,
                         const Session_mv<Session_impl_t>& val);

/**
 * Prints string representation of the given `Server_session_mv` to the given `ostream`.
 *
 * @relatesalso Server_session_mv
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Server_session_impl_t>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_mv<Server_session_impl_t>& val);

/**
 * Prints string representation of the given `Client_session_mv` to the given `ostream`.
 *
 * @relatesalso Client_session_mv
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Client_session_impl_t>
std::ostream& operator<<(std::ostream& os, const Client_session_mv<Client_session_impl_t>& val);

/**
 * Prints string representation of the given `Session_server` to the given `ostream`.
 *
 * @relatesalso Session_server
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
std::ostream& operator<<(std::ostream& os,
                         const Session_server
                                 <S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES, Mdt_payload>& val);

} // namespace ipc::session

/**
 * `sync_io`-pattern counterparts to async-I/O-pattern object types in parent namespace ipc::session.
 *
 * In this case they are given as only a small handful of adapter templates.  Just check out their doc headers.
 */
namespace ipc::session::sync_io
{

// Types.

template<typename Session>
class Server_session_adapter;
template<typename Session>
class Client_session_adapter;
template<typename Session_server>
class Session_server_adapter;

// Free functions.

/**
 * Prints string representation of the given `Server_session_adapter` to the given `ostream`.
 *
 * @relatesalso Server_session_adapter
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_adapter<Session>& val);

/**
 * Prints string representation of the given `Client_session_adapter` to the given `ostream`.
 *
 * @relatesalso Client_session_adapter
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session>
std::ostream& operator<<(std::ostream& os,
                         const Client_session_adapter<Session>& val);

/**
 * Prints string representation of the given `Session_server_adapter` to the given `ostream`.
 *
 * @relatesalso Session_server_adapter
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_server>
std::ostream& operator<<(std::ostream& os,
                         const Session_server_adapter<Session_server>& val);

} // namespace ipc::session::sync_io
