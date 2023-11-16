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
#include "ipc/session/detail/session_shared_name.hpp"
#include <mutex>
#include <regex>

namespace ipc::session
{

namespace
{

/// File-local helper variable: a regex used by decompose_conventional_shared_name() overload 1 (non-global-scope).
std::regex non_global_scope_name_regex;

/// File-local helper variable: a regex used by decompose_conventional_shared_name() overload 2 (global-scope).
std::regex global_scope_name_regex;

/// File-local helper variable: ensures #non_global_scope_name_regex is built thread-safely only once.
std::once_flag non_global_scope_name_regex_built;

/// File-local helper variable: ensures #global_scope_name_regex is built thread-safely only once.
std::once_flag global_scope_name_regex_built;

}; // namespace (anon)

Shared_name build_conventional_shared_name
              (const Shared_name& resource_type, const Shared_name& srv_app_name,
               const Shared_name& srv_namespace, const Shared_name& cli_app_name,
               const Shared_name& cli_namespace_or_sentinel)
{
  // Refer to Shared_name class doc header for the following.  Make sure the two always match when modifying either.

#ifndef NDEBUG
  const auto& SENTINEL = Shared_name::S_SENTINEL;
#endif

  assert(!srv_namespace.empty());
  assert(!cli_app_name.empty());
  assert(!cli_namespace_or_sentinel.empty());

  assert((srv_namespace != SENTINEL) && "Perhaps you want to use the other build_conventional_shared_name()?");
  assert((cli_app_name != SENTINEL) && "Perhaps you want to use the other build_conventional_shared_name()?  "
                                         "It does support non-SENTINEL srv_namespace.");

  auto name = build_conventional_shared_name_prefix(resource_type, srv_app_name);

  assert(name.has_trailing_separator());
  name += srv_namespace;

  name /= cli_app_name;
  name /= cli_namespace_or_sentinel;

  return name;
} // build_conventional_shared_name(1)

Shared_name build_conventional_shared_name
              (const Shared_name& resource_type, const Shared_name& srv_app_name,
               const Shared_name& srv_namespace_or_sentinel)
{
  // Refer to Shared_name class doc header for the following.  Make sure the two always match when modifying either.

  const auto& SENTINEL = Shared_name::S_SENTINEL;

  assert(!srv_namespace_or_sentinel.empty());

  auto name = build_conventional_shared_name_prefix(resource_type, srv_app_name);

  assert(name.has_trailing_separator());
  name += srv_namespace_or_sentinel;

  name /= SENTINEL;

  return name;
} // build_conventional_shared_name(2)

Shared_name build_conventional_shared_name_prefix(const Shared_name& resource_type, const Shared_name& srv_app_name)
{
  // Refer to Shared_name class doc header for the following.  Make sure the two always match when modifying either.

  const auto& ROOT_MAGIC = Shared_name::S_ROOT_MAGIC;
#ifndef NDEBUG
  const auto& SENTINEL = Shared_name::S_SENTINEL;
#endif

  assert(!resource_type.empty());
  assert(!srv_app_name.empty());
  assert((srv_app_name != SENTINEL) && "Perhaps you want to use build_conventional_non_session_based_shared_name()?");

  Shared_name name;
  name /= ROOT_MAGIC;
  name /= resource_type;
  name /= srv_app_name;
  name /= Shared_name();
  return name;
} // build_conventional_shared_name()

bool decompose_conventional_shared_name(const Shared_name& name,
                                        Shared_name* resource_type, Shared_name* srv_app_name,
                                        Shared_name* srv_namespace, Shared_name* cli_app_name,
                                        Shared_name* cli_namespace_or_sentinel, Shared_name* the_rest)
{
  using flow::util::ostream_op_string;
  using std::regex_match;
  using std::smatch;
  using std::string;

  /* This is thread-safe local `static`; actual local `static` is supposedly thread-safe in C++1x but with caveats.
   * So just ensure it.  Why not just make it global `static`?  Answer: it relies on extern `static`s which
   * may not yet be initialized (static init ordering problem).  Anyway it's nice to keep the regex string local. */
  std::call_once(non_global_scope_name_regex_built, [&]()
  {
    // Compatible with build_conventional_shared_name(1).
    non_global_scope_name_regex.assign
      (ostream_op_string(Shared_name::S_SEPARATOR, Shared_name::S_ROOT_MAGIC.str(), Shared_name::S_SEPARATOR,
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // resource_type
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // srv_app_name
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // srv_namespace
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // cli_app_name
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // cli_namespace_or_sentinel
                         "(.*)")); // the_rest
  });

  smatch matches;
  if (!regex_match(name.str(), matches, non_global_scope_name_regex))
  {
    return false;
  }
  // else

  // Just one little thing: srv_namespace is promised to never equal SENTINEL.  Hence:
  const auto srv_namespace_str = matches[3].str();
  if (srv_namespace_str == Shared_name::S_SENTINEL.str())
  {
    return false;
  }
  // else

  if (resource_type)
  {
    *resource_type = Shared_name::ct(matches[1].str());
  }
  if (srv_app_name)
  {
    *srv_app_name = Shared_name::ct(matches[2].str());
  }
  if (srv_namespace)
  {
    *srv_namespace = Shared_name::ct(srv_namespace_str);
  }
  if (cli_app_name)
  {
    *cli_app_name = Shared_name::ct(matches[4].str());
  }
  if (cli_namespace_or_sentinel)
  {
    *cli_namespace_or_sentinel = Shared_name::ct(matches[5].str());
  }
  if (the_rest)
  {
    *the_rest = Shared_name::ct(matches[6].str());
  }

  return true;
} // decompose_conventional_shared_name(1)

bool decompose_conventional_shared_name(const Shared_name& name,
                                        Shared_name* resource_type, Shared_name* srv_app_name,
                                        Shared_name* srv_namespace_or_sentinel, Shared_name* the_rest)
{
  using flow::util::ostream_op_string;
  using std::regex_match;
  using std::smatch;
  using std::string;

  // Analogous to the other overload, just simpler (fewer fields to decompose-to).  Keeping comments light.

  std::call_once(global_scope_name_regex_built, [&]()
  {
    // Compatible with build_conventional_shared_name(2).
    global_scope_name_regex.assign
      (ostream_op_string(Shared_name::S_SEPARATOR, Shared_name::S_ROOT_MAGIC.str(), Shared_name::S_SEPARATOR,
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // resource_type
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // srv_app_name
                         "([^", Shared_name::S_SEPARATOR, "]+)", Shared_name::S_SEPARATOR, // srv_namespace_or_sentinel
                         Shared_name::S_SENTINEL.str(), Shared_name::S_SEPARATOR,
                         "(.*)")); // the_rest
  });

  smatch matches;
  if (!regex_match(name.str(), matches, global_scope_name_regex))
  {
    return false;
  }
  // else

  if (resource_type)
  {
    *resource_type = Shared_name::ct(matches[1].str());
  }
  if (srv_app_name)
  {
    *srv_app_name = Shared_name::ct(matches[2].str());
  }
  if (srv_namespace_or_sentinel)
  {
    *srv_namespace_or_sentinel = Shared_name::ct(matches[3].str());
  }
  if (the_rest)
  {
    *the_rest = Shared_name::ct(matches[4].str());
  }

  return true;
} // decompose_conventional_shared_name(2)

} // namespace ipc::session
