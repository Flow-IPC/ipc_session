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
#include "ipc/common.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::session
{

// Types.

/**
 * Grouping of compile-time config/knobs (as of this writing: constants) governing some of the behavior of
 * ipc::session sessions.
 *
 * ### Rationale ###
 * Some of these could be viewed as implementation details; nevertheless they are public, much like
 * transport::Native_socket_stream_cfg: it is harmless, and it can be helpful for logging/reporting or for
 * understanding the system's behavior (e.g., how quickly a stuck opposing process is detected).
 *
 * @todo Session_cfg is the nucleus of a more general overhaul of ipc::session configuration (e.g., grouping
 * various `Session` / `Session_server` template parameters into a `struct`; possibly per-Client_app knobs).
 * Its constants may then become run-time and/or per-session-configurable.
 */
class Session_cfg
{
public:
  // Constants.

  /**
   * The period with which each session-internal channel -- notably the session master channel; also the
   * internal channel used by an arena-lending SHM-provider (as of this writing SHM-jemalloc) -- sends low-level
   * keep-alive pings to the opposing peer.  See transport::Channel::auto_ping().
   *
   * The opposing side uses the same value; hence #S_IDLE_TIMEOUT (which must comfortably exceed it) works
   * consistently in both directions.
   */
  static constexpr util::Fine_duration S_PING_PERIOD = boost::chrono::seconds{2};

  /**
   * The idle timeout of each session-internal channel (see #S_PING_PERIOD): if no low-level traffic -- including
   * the opposing side's pings -- arrives in this long, the channel is considered hosed; and hence the session is
   * too.  See transport::Channel::idle_timer_run().
   *
   * Its purpose is to detect an opposing process that is stuck (alive but not functioning), as opposed to dead
   * (detected immediately regardless).  As a false positive (hosing a healthy session over a transient stall)
   * is costlier than slower detection of a stuck peer, the value is generous relative to #S_PING_PERIOD.
   */
  static constexpr util::Fine_duration S_IDLE_TIMEOUT = boost::chrono::seconds{15};

private:
  // Constructors/destructor.

  /// Forbid instantiation, for it is pointless.
  Session_cfg() = delete;
}; // class Session_cfg

} // namespace ipc::session
