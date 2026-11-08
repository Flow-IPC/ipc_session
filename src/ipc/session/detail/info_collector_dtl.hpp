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

#include "ipc/session/info_collector.hpp"

namespace ipc::session
{

// Types.

/**
 * This is the `friend` facade of Info_collector that exposes the `private` constructor, so that only internal
 * session code (Client_session_impl, Server_session_impl) can construct a Info_collector.  The user receives
 * a pointer to an already-constructed Info_collector via Session::info_collector() but cannot construct one
 * themselves.
 *
 * @see Info_collector doc header.
 */
struct Info_collector_dtl
{
  // Methods.

  /**
   * Constructs a Info_collector capturing a `weak_ptr` to the given session master channel.
   *
   * @tparam Master_structured_channel_t
   *         Deduced from `master_channel`.
   * @param master_channel
   *        The SMC; must be non-null.
   * @return Newly constructed Info_collector.
   */
  template<typename Master_structured_channel_t>
  static Info_collector<Master_structured_channel_t>
    ct_base(const boost::shared_ptr<Master_structured_channel_t>& master_channel);
}; // struct Info_collector_dtl

// Template implementations.

template<typename Master_structured_channel_t>
Info_collector<Master_structured_channel_t> // Static.
  Info_collector_dtl::ct_base(const boost::shared_ptr<Master_structured_channel_t>& master_channel)
{
  return Info_collector<Master_structured_channel_t>(master_channel);
}

} // namespace ipc::session
