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

#include "ipc/util/util_fwd.hpp"
#include <boost/unordered_set.hpp>

namespace ipc::session
{

// Types.

/**
 * A *description* of an application in this ipc::session inter-process communication universe.  An application is,
 * at least roughly speaking, 1-1 with a distinct executable presumably interested in communicating with another such
 * executable.  A process is an instance of an application that has begun execution at some point.
 *
 * An App is a *description* of an application, and typically used (at least after program initialization, before
 * actual IPC begins) only as supertype of Client_app and/or Server_app, it is possible that 2+ `App`s exist but
 * describe one actual application but in different roles.  2+ such `App`s must have all members equal to each
 * other respectively.  In particular see App::m_name.
 *
 * This is a data store (and a simple one).  The same holds of Client_app and Server_app.  As of this writing
 * there are no rigid rules as to the existence of an App/Client_app/Server_app with this or that stored inside.
 * Make them all day long, if you want!  More restrictions shall come into play only when passing various
 * App objects and sub-objects (in some form) to ipc::session APIs, most notably the Session concept and buddies.
 *
 * However, to be useful, we suggest the following convention.  Some key ipc::session APIs may rely on it.
 *
 * ### App, Client_app, Server_app registry conventions ###
 * - Maintain a global (to the applications in your IPC universe) registry of known applications.  It could be
 *   stored in JSON or XML form in some central shared software component and read-into a master `set` or
 *   `unordered_set` (by App::m_name; see its doc header) available in a centrally-available library;
 *   or this `*set` could simply be a hard-coded C++ `extern const`; whatever works.
 * - In the current model supported by ipc::session: Suppose app Ap wants to communicate with app Bp; this is
 *   called the A-B *split*.  In any A-B split, one must be chosen as the *client*, the other as the *server*;
 *   let's by convention here assume they're always listed in server-client order.  However, it is allowed that
 *   two splits A-B and C-A co-exist in the same universe; meaning A is server when speaking to B but
 *   client when speaking to C.  Lastly, for each server app X, there is a global list of distinct client
 *   apps that are allowed to initiate IPC sessions with it (see ipc::Session concept).  Given that:
 *   - An App (in the above-described global registry) that is a client in any split shall be copied-into a
 *     (separate) Client_app object.
 *     - This global registry of known client applications shall similarly be maintained as a global `*set`.
 *       (The information regarding which apps are clients, too, can be read from JSON/XML/whatever or hard-coded.)
 *   - Similarly an App that is a server in any split shall be copied-into a (separate) Server_app object,
 *     forming the 3rd and final global `*set` of applications.
 *     - For each such Server_app, save the Client::m_name of each Client_app allowed to initiate sessions
 *       with it into its Server_app::m_allowed_client_apps.  (Again, this mapping can be read from JSON/XML/whatever
 *       or be hard-coded in some fashion.  In the end, though, it would go into `m_allowed_client_apps`.)
 *
 * Thus one would end up with, after initialization of each process wishing to participate in a given IPC universe:
 *   - An immutable `*set` of `App`s (global App registry).
 *   - An immutable `*set` of `Client_app`s (global Client_app registry).
 *   - An immutable `*set` of `Server_app`s (global Server_app registry), each Server_app listing the `m_name`s
 *     of `Client_app`s referring to the applications that may initiate sessions with it in its immutable
 *     Server_app::m_allowed_client_apps.
 *
 * There should be no need, from this moment on, to copy any App, Client_app, or Server_app.  It shall be passed
 * by `const` reference.
 */
struct App
{
  // Types.

  /// Suggested type for storing master repository or all `Apps`s.  See App doc header for discussion.
  using Master_set = boost::unordered_map<std::string, App>;

  // Data.

  /**
   * Brief application name, readable to humans and unique across all other applications' names; used
   * both as a short programmatic key in shared resource naming (including as a folder in #Shared_name for
   * kernel-persistent resources and addresses; and in path names -- such as PID file name -- in the file system).
   *
   * ### Convention: mandated use as key ###
   * Neither App nor any nearby sub-class or alias of App (Client_app, Server_app) shall be used as the key type
   * in an associative container (e.g., `set`, `unordered_map`).  Instead App::m_name shall be used as the key
   * when lookup by distinct application is desired.  Two scenarios come to mind:
   *   - You want to store a set of applications fitting some criterion/a, but this is not some master repository
   *     of App objects in the first place -- rather it is referring to some subset of such a master repository.
   *     - Then, store simply a `set` or `unordered_set` (etc.) of `std::string m_name`s.
   *       - Rationale: The basic desired operation here is an exitence-check (is application X present, based on
   *         some definition of present?).  #m_name is by definition guaranteed to be distinct from all others.
   *         Hence storing such strings alone is sufficient and reasonably efficient while also not relying on
   *         App address uniqueness for lookup (which, while efficient, is a pain to maintain).
   *   - You want to store some master repository of App objects, with all their config such as UID, GID, etc.
   *     - Then, store a `map` or `unordered_map`, with the keys being `std::string m_name`s,
   *       while the values are filled-out App objects (or objects of App sub-class) -- with the guarantee
   *       that `M[name].m_name == name` for all `name`s in any such `M`.
   *       - Rationale: Same as above; but in addition to being able to perform existence-check by #m_name,
   *         one can also iterate through the master details of all the applications and/or
   *         lookup a given application's master details.
   *
   * Due to these conventions:
   *   - We do not supply the usual complement of associative-container "stuff" for App and buddies:
   *     equality, less-than, hashing.  We do not want to encourage direct App storage that way.
   *   - The idea is: When such storage is desired, use that usual complement of associative-container "stuff"
   *     that is already supplied for `std::string`, as applied to App::m_name.
   *
   * Digression regarding rationale: Why use #m_name, and not (say) `const App*`, as the key in associative
   * lookups?  Answer: It was considered.  Due to the infrequency of such lookups, combined with the
   * debug-friendliness/expressiveness of referring to a distinct app by its user-readable name, the lookup-by-name
   * convention felt more straightforward and convenient.  Plus, the address of an object stored in a container
   * may change while it is being built, which would add an element of brittleness.  One could also use an
   * iterator as a key, but then the registry container would be mandated to be iteratator-stable-under-insertion,
   * and that brings an annoying host of worries.  Indexing by string name is solid by comparison, if a bit slower.
   *
   * ### Conventions: Naming ###
   * Must begin with an ASCII alphabetical character and otherwise consist of only ASCII alphanumerics and underscores.
   * While these are partially conventions driven by style (for example, dashes could also be allowed -- we just
   * do not want them to be, so that underscores are always the word-separator used consistently), the following
   * is an important guarantee for related #Shared_name and `fs::path` semantics:
   *   - Must not contain a util::Shared_name::S_SEPARATOR.
   *   - Must not contain a forward-slash or any other file system path separator.
   *   - Must not contain a dot or any other conceivable extension separator (other than underscore).
   *
   * Therefore:
   *   - In #Shared_name paths one can embed #m_name by preceding and/or succeeding it with a
   *     util::Shared_name::S_SEPARATOR, thus making it a full or partial folder name.
   *   - In file system paths one can embed #m_name by preceding and/or succeeding it with a
   *     forward-slash, thus making it a full or partial directory name.
   *   - In either: one can embed #m_name by preceding and/or succeeding it with a dot as an extension separator.
   *
   * So if #m_name is "abc_def" one can do things like "/some/dir/abc_def/some_file" and "/some/dir/abc_def.pid/file"
   * and "/some/dir/prefix.abc_def.3".  I.e., #m_name can be safely used as a conceptual "token," even when
   * combined with path/name separators and dot-extension naming.
   */
  std::string m_name;

  /**
   * Absolute, lexically normalized canonical path to the executable entity (which is not a directory), as it
   * would appear in a command line or `exec()`-like invocation when actually invoking the application.
   * Formally (refer to cppreference.com `filesystem::path` doc page):
   *   - There is no root-name: No Windows drive letters ("C:"), no network file share ("//share_name"), etc.
   *   - It is a Unix path: Only forward-slash is used as separator, and that character always is a separator.
   *   - It is absolute: It starts with forward-slash.
   *   - It is lexically normalized: See reference, but essentially it means no more than one slash in a row,
   *     no dot components, no dot-dot components.  (Note: dots and dot-dots can *appear* -- but an *entire*
   *     component cannot comprise solely such a sequence; where component means: a thing preceded by slash and
   *     succeeded by slash or end-of-path.)
   *
   * Do note that because of the "as it would appear in a command line..." requirement this indirectly restricts
   * how processes are to be invoked in this system: always via 1, absolute, lexically normal path to the executable.
   * (Do also note this is still not necessary force an unambiguous way to invoke an application installed at
   * a given location: sym-links, at least, introduce an infinite number of ways to refer to the same binary.
   * That said, at least lexical normalization can be done programmatically in `fs`, and sym-link following
   * path resolution is also separately supplied by `fs`.)
   */
  fs::path m_exec_path;

  /// The application must run as this user ID (UID).  Files and other shared resources shall have this owner.
  util::user_id_t m_user_id;

  /// The application must run as this group ID (GID).  Files and other shared resources shall have this owner.
  util::group_id_t m_group_id;
}; // struct App

/**
 * An App that is used as a client in at least one client-server IPC split.  This is explained in the
 * "App, Client_app, Server_app registry conventions" section of the `struct` App doc header.
 *
 * @see `struct` App doc header.
 *
 * As of this writing a Client_app is just an App with no added stored information.  That said a Server_app
 * may exist whose App base object is equals to a Client_app `*this`'s App base object (in particular App::m_name
 * but also all other members).
 */
struct Client_app : public App
{
  // Types.

  /// Short-hand for base type.
  using Base = App;

  /// Suggested type for storing master repository or all `Client_apps`s.  See App doc header for discussion.
  using Master_set = boost::unordered_map<std::string, Client_app>;
};

/**
 * An App that is used as a server in at least one client-server IPC split.  This is explained in the
 * "App, Client_app, Server_app registry conventions" section of the `struct` App doc header.
 *
 * @see `struct` App doc header.
 *
 * As explained there, each Server_app -- in addition to the source App fields copied from an App in the master
 * App repository -- ultimately stores references (by App::m_name) to the `Client_app`s describing client apps
 * that may initiate sessions with that Server_app.
 */
struct Server_app : public App
{
  // Types.

  /// Suggested type for storing master repository or all `Server_apps`s.  See App doc header for discussion.
  using Master_set = boost::unordered_map<std::string, Server_app>;

  /// Short-hand for existence-checkable set of `Client_app`s via App::m_name.
  using Client_app_set = boost::unordered_set<std::string>;

  // Data.

  /**
   * A given Client_app (as identified by its distinct App::m_name) may request to establish an IPC session
   * with an instance of `*this` Server_app as the conversation partner process if and only if
   * it is in this set.
   */
  Client_app_set m_allowed_client_apps;

  /**
   * Absolute path to the directory (without trailing separator) in the file system where kernel-persistent
   * runtime, but not temporary, information shall be placed for this particular application; leave empty to
   * use the default system path instead.  Informally: In production it is recommended to leave this
   * empty, as the default system path is a well known location (namely /var/run) where one would expect to find
   * such files.
   *
   * Kernel-persistent means that it'll disappear at reboot; runtime, but not temporary, means it's... not a
   * thing that might be wiped spuriously anytime (informally: not /tmp).  Informally: just know that it is normally
   * /var/run, and that it stores things such as process-ID (PID) files.
   *
   * It shall be a Unix path and absolute (starts with forward-slash) (and lacks a root-name).  Informally it is
   * probably best for it to be lexically normal as well.
   *
   * ### Rationale ###
   * Why allow this override, if the default is sensible?  Answer: The default usually requires admin privileges
   * for writing.  For test/debug purposes a location like /tmp or /tmp/var/run can be convenient.
   */
  fs::path m_kernel_persistent_run_dir_override;

  /**
   * Specifies level of access for `Client_app`s (which must, also, be in #m_allowed_client_apps at any rate)
   * as enforced by file system permissions.
   *
   * Specifically, as of this writing, it determines at least the following items.  Note that ownership is
   * identified by the UID/GID settings (matching `*this` App::m_user_id and App::m_group_id) of various
   * files and other shared resources created by the server* application in the ipc::session framework.
   * (* - Actually, at times, resources are created by the client application too.  See below.)
   *   - File permissions set on the CNS (PID) file established by Session_server.  A given #Client_session
   *     (etc.) must read this file in order to know to which Session_server to issue a session-open request
   *     (establish a Session); if it cannot access that file, it cannot open a session against this
   *     Server_app.
   *     - If this check fails, and everything is functioning as generally intended internally, then the below
   *       items are moot (as opening a session is a prerequisite for anything else).
   *   - Permissions set on any SHM pool created for a given `"shm::*::Server_session"`. and
   *     `"shm::*::Session_server"`. A Client_app may not be able to complete its session-open attempt
   *     (shm::classic::Client_session::sync_connect(), etc.) if its user lacks the permissions to open the
   *     underlying SHM-pool(s) resource in the file system.(It also conceivably may be able to complete the
   *     session-open but fail if opening a pool on-demand later; the session will then likely end
   *     prematurely.  This depends on the inner workings of the particular SHM system chosen;
   *     shm::classic::Client_session opens pools during session-open procedure exclusively, but other
   *     systems may behave differently.)
   *     - Arena-lending SHM providers -- as of this writing namely ipc::shm::arena_lend::jemalloc -- involve
   *       both sides (session-server and session-client) each creating SHM pool(s) for allocations from
   *       within their respective processes.  #m_permissions_level_for_client_apps applies to the
   *       server-created pools, yes; but also client-created pools.  At the moment it seems to me
   *       (ygoldfel) that nevertheless a common setting in Server_app still makes sense.  In practice, it
   *       means this: If this is set to util::Permissions_level::S_USER_ACCESS, then the Client_app and
   *       Server_app UID/GID shall need to be equal; if util::Permissions_level::S_GROUP_ACCESS then just
   *       the GID (while UIDs ideally would be different).(Obviously if `S_UNRESTRICTED` then does not
   *       matter.)  Don't see the sense in making this a separate knob in Client_app.  Maybe though?  Not a
   *       formal to-do for now.
   *   - Similarly (to preceding bullet point, minus the `arena_lend` caveat) for bipc MQs.
   *     ipc::session opens such MQs (if so configured at compile-time) during session-open; hence on permissions
   *     failure the session-open will itself fail.
   *   - Similarly (to preceding bullet point) for POSIX MQs.
   *
   * As of this writing the list is complete; however it is conceivable it may be analogously extended to more
   * resources.
   * @internal
   * That list is actually not quite complete even as of this writing.  The CNS (PID) file's shared-mutex
   * is also subject to this, as are other similar shared-mutexes (in Linux, semaphores).  However that's very
   * Inside Baseball to mention in public docs.
   */
  util::Permissions_level m_permissions_level_for_client_apps;
}; // struct Server_app

// Free functions: in *_fwd.hpp.

} // namespace ipc::session
