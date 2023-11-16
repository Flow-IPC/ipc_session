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
#include "ipc/session/app.hpp"
#include "ipc/session/session_fwd.hpp"
#include "ipc/session/error.hpp"
#include "ipc/util/native_handle.hpp"
#include <flow/error/error.hpp>
#include <sys/stat.h>

namespace ipc::session
{

// Implementations.

void ensure_resource_owner_is_app(flow::log::Logger* logger_ptr, const fs::path& path, const App& app,
                                  Error_code* err_code)
{
  using util::Native_handle;
  using boost::system::system_category;
  using ::open;
  using ::close;
  // using ::O_PATH; // It's a macro apparently.
  // using ::errno; // It's a macro apparently.

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { ensure_resource_owner_is_app(logger_ptr, path, app, actual_err_code); },
         err_code, "session::ensure_resource_owner_is_app(1)"))
  {
    return;
  }
  // else

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_SESSION);

  /* We can either reuse the handle-based overload, or we can do stat() instead of fstat().  The former seems nicer;
   * though we'll have to open a handle temporarily.  I've seen Boost.interprocess do it fairly casually for
   * similar purposes, so why not.  The O_PATH flag (which opens the resource just for this purpose -- not for
   * I/O) is perfect for it, particularly since the resource might not be a file but a SHM pool, etc. */

  int native_handle = open(path.c_str(), O_PATH);
  if (native_handle == -1)
  {
    *err_code = Error_code(errno, system_category());
    FLOW_LOG_WARNING("Tried to check ownership of resource at [" << path << "] but while obtaining info-only handle "
                     "encountered error [" << *err_code << "] [" << err_code->message() << "]; unable to check.");
    return;
  }
  // else
  Native_handle handle(native_handle);

  // For nicer messaging add some more logging on error.  A little code duplication, but it's OK.
  ensure_resource_owner_is_app(logger_ptr, handle, app, err_code);
  if (*err_code)
  {
    FLOW_LOG_WARNING("Check of ownership of resource at [" << path << "], upon successfully opening probe-only "
                     "descriptor/handle, resulted in error in checking or "
                     "unexpected ownership; see preceding WARNING referencing all other details.");
  }

  close(native_handle);
} // ensure_resource_owner_is_app(1)

void ensure_resource_owner_is_app(flow::log::Logger* logger_ptr, util::Native_handle handle, const App& app,
                                  Error_code* err_code)
{
  using boost::system::system_category;
  using ::fstat;
  /* using Stats = struct ::stat; // This worked at coliru.com but not here; not sure why.
   *                              // The crux of this whole thing is that struct ::stat() and ::stat()
   *                              // have the same name.  Blech.  Just specify it explicitly below, C-style. */
  // using ::errno; // It's a macro apparently.

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { ensure_resource_owner_is_app(logger_ptr, handle, app, actual_err_code); },
         err_code, "session::ensure_resource_owner_is_app(2)"))
  {
    return;
  }
  // else

  assert((!handle.null()) && "Disallowed per contract.");

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_SESSION);

  // There's no Boost.filesystem or STL way to get UID/GID, that I know of (even via path); so use POSIX-y thing.
  struct ::stat stats;
  const auto rc = fstat(handle.m_native_handle, &stats);
  if (rc == -1)
  {
    *err_code = Error_code(errno, system_category());
    FLOW_LOG_WARNING("Tried to check ownership via descriptor/handle [" << handle << "] but encountered "
                     "error [" << *err_code << "] [" << err_code->message() << "]; unable to check.");
  }
  else if ((stats.st_uid != app.m_user_id) || (stats.st_gid != app.m_group_id))
  {
    *err_code = error::Code::S_RESOURCE_OWNER_UNEXPECTED;
    FLOW_LOG_WARNING("Checked ownership via descriptor/handle [" << handle << "] but encountered "
                     "error [" << *err_code << "] [" << err_code->message() << "]; unable to check.");
  }
  else
  {
    err_code->clear();
  }
} // ensure_resource_owner_is_app(2)

std::ostream& operator<<(std::ostream& os, const App& val)
{
  return os << '[' << val.m_name << "] "
               "exec[" << val.m_exec_path << "] user[" << val.m_user_id << ':' << val.m_group_id << ']';
}
std::ostream& operator<<(std::ostream& os, const Client_app& val)
{
  return os << static_cast<const App&>(val);
}

std::ostream& operator<<(std::ostream& os, const Server_app& val)
{
  using boost::algorithm::join;

  return os << static_cast<const App&>(val) << " allowed_cli_apps[" << join(val.m_allowed_client_apps, " ") << ']';
}

} // namespace ipc::session
