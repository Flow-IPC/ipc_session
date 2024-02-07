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
#include "ipc/session/error.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::session::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::session module.  Analogous to
 * transport::error::Category.  All notes therein apply.
 */
class Category :
  public boost::system::error_category
{
public:
  // Constants.

  /// The one Category.
  static const Category S_CATEGORY;

  // Methods.

  /**
   * Analogous to transport::error::Category::name().
   *
   * @return See above.
   */
  const char* name() const noexcept override;

  /**
   * Analogous to transport::error::Category::name().
   *
   * @param val
   *        See above.
   * @return See above.
   */
  std::string message(int val) const override;

  /**
   * Analogous to transport::error::Category::name().
   * @param code
   *        See above.
   * @return See above.
   */
  static util::String_view code_symbol(Code code);

private:
  // Constructors.

  /// Boring constructor.
  explicit Category();
}; // class Category

// Static initializations.

const Category Category::S_CATEGORY;

// Implementations.

Error_code make_error_code(Code err_code)
{
  /* Assign Category as the category for flow::error::Code-cast error_codes;
   * this basically glues together Category::name()/message() with the Code enum. */
  return Error_code(static_cast<int>(err_code), Category::S_CATEGORY);
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "ipc/session";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  // See notes in transport::error in same spot.
  switch (static_cast<Code>(val))
  {
  case Code::S_INVALID_ARGUMENT:
    return "User called an API with 1 or more arguments against the API spec.";
  case Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER:
    return "Async completion handler is being called prematurely, because underlying object is shutting down, "
           "as user desires.";
  case Code::S_CLIENT_NAMESPACE_STORE_BAD_FORMAT:
    return "Session master channel: log-in as client: The Client Namespace Store (CNS) file (a/k/a PID file) lacks "
           "leading newline-terminated line with numeric PID.";
  case Code::S_CLIENT_MASTER_LOG_IN_RESPONSE_BAD:
    return "Session master channel: log-in as client: received log-in response is not the expected internal message.";
  case Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN:
    return "Session master channel: log-in as server: client identifies self as unknown client-app or one that is not "
           "registered as an allowed partner to this server-app.";
  case Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS:
    return "Session master channel: log-in as server: client application-level process credentials (UID, etc.) "
           "do not match the registered values and/or the OS-reported client peer values from socket stream.";
  case Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN:
    return "Session channel opening: User code chose to reject all passive-open requests.";
  case Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE:
    return "Session channel opening: Server peer is responsible for low-level resource acquisition but failed "
             "to do so.  Session continues; this is not a fatal error.";
  case Code::S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT:
    return "Session channel opening: "
             "Other peer must reply quickly enough as-if non-blocking but did not do so in time.";
  case Code::S_MUTEX_BIPC_MISC_LIBRARY_ERROR:
    return "Low-level boost.ipc.mutex: boost.interprocess emitted miscellaneous library exception sans a system code; "
           "a WARNING message at throw-time should contain all possible details.";
  case Code::S_SERVER_MASTER_POST_LOG_IN_SEND_FAILED_MISC:
    return "Session master channel: post-log-in setup (such as SHM setup): send failed because channel hosed "
           "around that time.  Timing was such that exact reason for this could not be obtained, but "
           "logs will show it.";
  case Code::S_SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH:
    return "Session master channel: log-in as server: opposing client's compile-time type config differs from "
           "local server's counterpart config; did you use the proper Server/Client_session paired templates and "
           "mutually equal template parameters?";
  case Code::S_RESOURCE_OWNER_UNEXPECTED:
    return "A resource in the file system (file, SHM pool, MQ, etc.) has or could have unexpected owner; "
           "ipc::session may emit this when acquiring resources and/or opening session if the server process "
           "is not running as the configured user, or a previous iteration was not running as that user.";

  case Code::S_END_SENTINEL:
    assert(false && "SENTINEL: Not an error.  "
                    "This Code must never be issued by an error/success-emitting API; I/O use only.");
  }
  assert(false);
  return "";
} // Category::message()

util::String_view Category::code_symbol(Code code) // Static.
{
  // Note: Must satisfy istream_to_enum() requirements.

  switch (code)
  {
  case Code::S_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER:
    return "OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER";
  case Code::S_CLIENT_NAMESPACE_STORE_BAD_FORMAT:
    return "CLIENT_NAMESPACE_STORE_BAD_FORMAT";
  case Code::S_CLIENT_MASTER_LOG_IN_RESPONSE_BAD:
    return "CLIENT_MASTER_LOG_IN_RESPONSE_BAD";
  case Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN:
    return "SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN";
  case Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS:
    return "SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS";
  case Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN:
    return "SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN";
  case Code::S_SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE:
    return "SESSION_OPEN_CHANNEL_SERVER_CANNOT_PROCEED_RESOURCE_UNAVAILABLE";
  case Code::S_SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT:
    return "SESSION_OPEN_CHANNEL_ACTIVE_TIMEOUT";
  case Code::S_MUTEX_BIPC_MISC_LIBRARY_ERROR:
    return "MUTEX_BIPC_MISC_LIBRARY_ERROR";
  case Code::S_SERVER_MASTER_POST_LOG_IN_SEND_FAILED_MISC:
    return "SERVER_MASTER_POST_LOG_IN_SEND_FAILED_MISC";
  case Code::S_SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH:
    return "SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH";
  case Code::S_RESOURCE_OWNER_UNEXPECTED:
    return "RESOURCE_OWNER_UNEXPECTED";

  case Code::S_END_SENTINEL:
    return "END_SENTINEL";
  }
  assert(false);
  return "";
}

std::ostream& operator<<(std::ostream& os, Code val)
{
  // Note: Must satisfy istream_to_enum() requirements.
  return os << Category::code_symbol(val);
}

std::istream& operator>>(std::istream& is, Code& val)
{
  /* Range [<1st Code>, END_SENTINEL); no match => END_SENTINEL;
   * allow for number instead of ostream<< string; case-insensitive. */
  val = flow::util::istream_to_enum(&is, Code::S_END_SENTINEL, Code::S_END_SENTINEL, true, false,
                                    Code(S_CODE_LOWEST_INT_VALUE));
  return is;
}

} // namespace ipc::session::error
