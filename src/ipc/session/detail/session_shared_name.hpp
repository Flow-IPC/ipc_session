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
#include "ipc/util/shared_name.hpp"

namespace ipc::session
{

/* Normally we'd put these prototypes into detail/session_fwd.hpp nearby; but there are some minor subtleties
 * preventing this.  For one there's a reference to Shared_name::S_SENTINEL as a defaul arg, which means
 * #include "ipc/util/shared_name.hpp" would be needed -- not cool in a _fwd.hpp.  Secondly there's the
 * many references to Shared_name -- which session_fwd.hpp aliases to util::Shared_name -- but session_fwd.hpp
 * needs detail/session_fwd.hpp for other reasons; so this creates a circular issue.  Because of these,
 * it's not in detail/session_fwd.hpp; nor in some kind of seaparate session_shared_name_fwd.hpp either.
 *
 * It's not really a huge deal, but it *is* a break from the recommended _fwd.hpp convention; seemed worth
 * explaining. */

// Free functions.

/**
 * Builds an absolute name according to the path convention explained in Shared_name class doc header;
 * this overload applies to non-global-scope resources within the ipc::session paradigm.  Typically one
 * would then add a util::Shared_name::S_SEPARATOR and then purpose-specific path component(s) (via `/=` or
 * similar).
 *
 * Behavior is undefined if a path fragment argument is empty (assertion may trip).
 *
 * @param resource_type
 *        Resource type constant.  Typically it is available as a `S_RESOURCE_TYPE_ID` constant in some class;
 *        otherwise as a `Shared_name::S_RESOURCE_TYPE_ID_*` constant.
 * @param srv_app_name
 *        Session-server application name; typically from Server_app::m_name.
 * @param srv_namespace
 *        The server namespace, uniquely identifying the server application *instance* (process) across all time,
 *        for the given `srv_app_name`.  (I.e., it need only be unique given the particular `resource_type` and
 *        `srv_app_name`.)
 * @param cli_app_name
 *        Session-client application name; typically from Client_app::m_name.
 * @param cli_namespace_or_sentinel
 *        The client namespace, uniquely identifying the client application *instance* (process), for the given
 *        set of values in preceding args.  (I.e., it need only be unique given the particular values combo in
 *        the preceding args.)  If the name applies to all instances of `cli_app_name` this shall be
 *        util::Shared_name::S_SENTINEL.
 * @return Absolute path not ending in `S_SEPARATOR`, typically to be appended with a `S_SEPARATOR` and more
 *         component(s) by the caller.
 */
Shared_name build_conventional_shared_name(const Shared_name& resource_type, const Shared_name& srv_app_name,
                                           const Shared_name& srv_namespace, const Shared_name& cli_app_name,
                                           const Shared_name& cli_namespace_or_sentinel
                                             = Shared_name::S_SENTINEL);

/**
 * Builds an absolute name according to the path convention explained in Shared_name class doc header;
 * this overload applies to global-scope resources within the ipc::session paradigm.  Typically one
 * would then add a util::Shared_name::S_SEPARATOR and then purpose-specific path component(s)
 * (via `/=` or similar).  Note that this overload supports both of the following situations:
 *   - Applicable to a particular Server_app *instance* (process).
 *   - Applicable to *all* such instances (processes).  Then set `srv_namespace_or_sentinel = S_SENTINEL`.
 *
 * Arguably the former is more common.
 *
 * Behavior is undefined if a path fragment argument is empty (assertion may trip).
 *
 * @param resource_type
 *        Resource type constant.  Typically it is available as a `S_RESOURCE_TYPE_ID` constant in some class;
 *        otherwise as a `Shared_name::S_RESOURCE_TYPE_ID_*` constant.
 * @param srv_app_name
 *        Session-server application name; typically from Server_app::m_name.
 * @param srv_namespace_or_sentinel
 *        The server namespace, uniquely identifying the server application *instance* (process) across all time,
 *        for the given `srv_app_name`.  (I.e., it need only be unique given the particular `resource_type` and
 *        `srv_app_name`.)  Alternatively it may equal `S_SENTINEL`, indicating the resource applies to
 *        *all* instances of `srv_app_name` (unique Server_app).
 * @return Absolute path not ending in `S_SEPARATOR`, typically to be appended with a `S_SEPARATOR` and more
 *         component(s) by the caller.
 */
Shared_name build_conventional_shared_name(const Shared_name& resource_type, const Shared_name& srv_app_name,
                                           const Shared_name& srv_namespace_or_sentinel
                                             = Shared_name::S_SENTINEL);

/**
 * Decomposes a Shared_name built as-if by build_conventional_shared_name() overload 1 (non-global-scope); setting
 * the various component values as out-args; or returning `false` if the name does not follow the
 * conventions of that build_conventional_shared_name() overload.
 *
 * Important: In order for this to return `true` (and thus optionally set any out-args), `name` must
 * be as-if it is a concatenation of:
 *   - The build_conventional_shared_name() (overload 1/non-global-scope) result.
 *   - `S_SEPARATOR`.
 *   - 0 or more characters (which are placed into `*the_rest` if not-null).
 *
 * I.e., `name` cannot be the result of build_conventional_shared_name() alone but also a separator and possibly
 * more characters.
 *
 * @param name
 *        Name to check/decompose.
 * @param resource_type
 *        If not-null, pointee is set to `resource_type` as-if given to build_conventional_shared_name() overload 1
 *        (non-global-scope).  If null ignored.
 * @param srv_app_name
 *        If not-null, pointee is set to `srv_app_name` as-if given to build_conventional_shared_name() overload 1
 *        (non-global-scope).  If null ignored.
 * @param srv_namespace
 *        If not-null, pointee is set to `srv_namespace` as-if given to build_conventional_shared_name() overload 1
 *        (non-global-scope).  If null ignored.  This will never equal `S_SENTINEL`.
 * @param cli_app_name
 *        If not-null, pointee is set to `cli_app_name` as-if given to build_conventional_shared_name() overload 1
 *        (non-global-scope).  If null ignored.
 * @param cli_namespace_or_sentinel
 *        If not-null, pointee is set to `cli_namespace_or_sentinel` as-if given to build_conventional_shared_name()
 *        overload 1 (non-global-scope).  If null ignored.  This may equal `S_SENTINEL`.
 * @param the_rest
 *        If not-null, pointee is set to whatever follows the above components (even if some or all of their out-args
 *        were null) and the `S_SEPARATOR` immediately following the last of the above components.
 *        To be clear: that `S_SEPARATOR` itself is not included as the leading character of `*the_rest`.
 *        If null ignored.  This may be `.empty()`.
 * @return `true` if and only if the name could be decomposed, meaning it was built as described above.
 */
bool decompose_conventional_shared_name(const Shared_name& name,
                                        Shared_name* resource_type, Shared_name* srv_app_name,
                                        Shared_name* srv_namespace, Shared_name* cli_app_name,
                                        Shared_name* cli_namespace_or_sentinel, Shared_name* the_rest);

/**
 * Decomposes a Shared_name built as-if by build_conventional_shared_name() overload 2 (global-scope); setting
 * the various component values as out-args; or returning `false` if the name does not follow the
 * conventions of that build_conventional_shared_name() overload.
 *
 * Important: In order for this to return `true` (and thus optionally set any out-args), `name` must
 * be as-if it is a concatenation of:
 *   - The build_conventional_shared_name() (overload 2/global-scope) result.
 *   - `S_SEPARATOR`.
 *   - 0 or more characters (which are placed into `*the_rest` if not-null).
 *
 * I.e., `name` cannot be the result of build_conventional_shared_name() alone but also a separator and possibly
 * more characters.
 *
 * @param name
 *        Name to check/decompose.
 * @param resource_type
 *        If not-null, pointee is set to `resource_type` as-if given to build_conventional_shared_name() overload 2
 *        (global-scope).  If null ignored.
 * @param srv_app_name
 *        If not-null, pointee is set to `srv_app_name` as-if given to build_conventional_shared_name() overload 2
 *        (global-scope).  If null ignored.
 * @param srv_namespace_or_sentinel
 *        If not-null, pointee is set to `srv_namespace_or_sentinel` as-if given to build_conventional_shared_name()
 *        overload 2 (global-scope).  If null ignored.  This will may equal `S_SENTINEL`.
 * @param the_rest
 *        If not-null, pointee is set to whatever follows the above components (even if some or all of their out-args
 *        were null), the `S_SENTINEL`, and the `S_SEPARATOR` immediately following the last of the above components.
 *        To be clear: that `S_SEPARATOR` itself is not included as the leading character of `*the_rest`.
 *        If null ignored.  This may be `.empty()`.
 * @return `true` if and only if the name could be decomposed, meaning it was built as described above.
 *         If `false` none of the out-args are touched.
 */
bool decompose_conventional_shared_name(const Shared_name& name,
                                        Shared_name* resource_type, Shared_name* srv_app_name,
                                        Shared_name* srv_namespace_or_sentinel, Shared_name* the_rest);

/**
 * Return the prefix common to all calls to either build_conventional_shared_name() overload with
 * the args `resource_type` and `srv_app_name`.  For example one can use a value built by this function as the
 * util::remove_each_persistent_with_name_prefix() prefix arg.
 *
 * Note that the returned value *always* ends with `S_SEPARATOR`.
 *
 * @param resource_type
 *        See build_conventional_shared_name() (either overload).
 * @param srv_app_name
 *        See build_conventional_shared_name() (either overload).
 * @return Absolute path prefix ending `S_SEPARATOR`.
 */
Shared_name build_conventional_shared_name_prefix(const Shared_name& resource_type, const Shared_name& srv_app_name);

} // namespace ipc::session
