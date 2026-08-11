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
#include <utility>

namespace ipc::session
{

// Types.

/**
 * This is the `friend` facade of Server_session or any more-advanced (e.g., SHM-capable) variant thereof
 * that exposes `private` APIs hidden from public user by providing public access to them; this is used internally
 * by Session_server.  The background is briefly explained in the impl section of Server_session doc header.
 *
 * @tparam Base_t
 *         The type of object whose specific `private` API to expose.
 */
template<typename Base_t>
struct Server_session_dtl
{
  // Types.

  /// Short-hand for wrapped class.
  using Base = Base_t;

  /// See #Base counterpart.
  using Session_base_obj = typename Base::Session_base_obj;

  // Data.

  /// Direct-initializable wrapped object.  Access `public` API through this reference; `private` API via `*this`.
  Base& m_base;

  // Methods.

  /**
   * Forwards to #Base ctor(s).
   * @param ctor_args
   *        See above.
   * @return New #Base.
   */
  template<typename... Ctor_args>
  static auto ct_base(Ctor_args&&... ctor_args) -> Base;
  // @todo See definition for reason for the odd --^-- signature form (Doxygen).

  /**
   * See #Base counterpart.
   * @param args
   *        See above.
   */
  template<typename... Args>
  void async_accept_log_in(Args&&... args);

  /**
   * See #Base counterpart.
   * @return See above.
   */
  const Session_base_obj& base() const;
}; // struct Server_session_dtl

// Template implementations.

template<typename Base_t>
template<typename... Ctor_args>
auto Server_session_dtl<Base_t>::ct_base(Ctor_args&&... ctor_args) -> Base_t
// Doxygen 1.9.4 gets confused here otherwise; the `->` form is a work-around for that.  @todo Revisit with later ver.
{
  return Base{std::forward<Ctor_args>(ctor_args)...};
}

template<typename Base_t>
template<typename... Args>
void Server_session_dtl<Base_t>::async_accept_log_in(Args&&... args)
{
  m_base.async_accept_log_in(std::forward<Args>(args)...);
}

template<typename Base_t>
const typename Server_session_dtl<Base_t>::Session_base_obj&
  Server_session_dtl<Base_t>::base() const
{
  return m_base.base();
}

} // namespace ipc::session
