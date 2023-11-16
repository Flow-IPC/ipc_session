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

#include "ipc/common.hpp"

/**
 * Namespace containing the ipc::session module's extension of boost.system error conventions, so that that API
 * can return codes/messages from within its own new set of error codes/messages.  Historically this was written
 * after ipc::transport::error, and essentially all the notes in that doc header and otherwise within that
 * namespace apply equally here.  Therefore please:
 *
 * @see ipc::transport::error documentation; notes therein (such as to-dos) likely apply here equally.
 */
namespace ipc::session::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/**
 * All possible errors returned (via `Error_code` arguments) by ipc::session functions/methods *outside of*
 * ipc::transport-triggered errors involved in transport involved in doing session-related work; and possibly
 * system-triggered errors.
 *
 * All notes from transport::error::Code doc header apply here.
 */
enum class Code
{
  /// User called an API with 1 or more arguments against the API spec.
  S_INVALID_ARGUMENT = S_CODE_LOWEST_INT_VALUE,

  /// Async completion handler is being called prematurely, because underlying object is shutting down, as user desires.
  S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER,

  /**
   * Session master channel: log-in as client: The Client Namespace Store (CNS) file (a/k/a PID file) lacks
   * leading newline-terminated line with numeric PID.
   */
  S_CLIENT_NAMESPACE_STORE_BAD_FORMAT,

  /// Session master channel: log-in as client: received log-in response is not the expected internal message.
  S_CLIENT_MASTER_LOG_IN_RESPONSE_BAD,

  /**
   * Session master channel: log-in as server: client identifies self as unknown client-app or one that is not
   * registered as an allowed partner to this server-app.
   */
  S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN,

  /**
   * Session master channel: log-in as server: client application-level process credentials (UID, etc.)
   * do not match the registered values and/or the OS-reported client peer values from socket stream.
   */
  S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS,

  /// Session channel opening: User code chose to reject all passive-open requests.
  S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN,

  /// Session channel opening: Server peer is responsible for low-level resource acquisition but failed to do so.
  S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE,

  /**
   * Session channel opening: Other peer must reply quickly enough as-if non-blocking but did not do so in time.
   * Session continues; this is not a fatal error.
   */
  S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT,

  /**
   * Low-level Boost.ipc.mutex: boost.interprocess emitted miscellaneous library exception sans a system code;
   * a WARNING message at throw-time should contain all possible details.
   */
  S_MUTEX_BIPC_MISC_LIBRARY_ERROR,

  /**
   * Session master channel: post-log-in setup (such as SHM setup): send failed because channel hosed around that time.
   * Timing was such that exact reason for this could not be obtained, but logs will show it.
   */
  S_SERVER_MASTER_POST_LOG_IN_SEND_FAILED_MISC,

  /**
   * Session master channel: log-in as server: opposing client's compile-time type config differs from
   * local server's counterpart config; did you use the proper Server/Client_session paired templates and
   * mutually equal template parameters?
   */
  S_SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH,

  /**
   * A resource in the file system (file, SHM pool, MQ, etc.) has or could have unexpected owner; ipc::session may
   * emit this when acquiring resources and/or opening session if the server process is not running as the
   * configured user, or a previous iteration was not running as that user.
   */
  S_RESOURCE_OWNER_UNEXPECTED,

  /// SENTINEL: Not an error.  This Code must never be issued by an error/success-emitting API; I/O use only.
  S_END_SENTINEL
}; // enum class Code

// Free functions.

/**
 * Analogous to transport::error::make_error_code().
 *
 * @param err_code
 *        See above.
 * @return See above.
 */
Error_code make_error_code(Code err_code);

/**
 * Analogous to transport::error::operator>>().
 *
 * @param is
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::istream& operator>>(std::istream& is, Code& val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

/**
 * Analogous to transport::error::operator<<().
 *
 * @param os
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::ostream& operator<<(std::ostream& os, Code val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

} // namespace ipc::session::error

namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets `value` to `false`, so that random arbitary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the offical way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::ipc::session::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  // See note in similar place near transport::error.
};

} // namespace boost::system
