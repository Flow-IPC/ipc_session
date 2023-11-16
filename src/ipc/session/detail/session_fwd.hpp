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

#include "ipc/session/schema/common.capnp.h"
#include <ostream>

namespace ipc::session
{

// Types.

// Find doc headers near the bodies of these compound types.

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload>
class Session_base;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload,
         schema::ShmType S_SHM_TYPE_OR_NONE = schema::ShmType::NONE,
         size_t S_SHM_MAX_HNDL_SZ = 0>
class Server_session_impl;

template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload,
         schema::ShmType S_SHM_TYPE_OR_NONE = schema::ShmType::NONE>
class Client_session_impl;

template<typename Server_session_t>
class Server_session_dtl;

template<typename Session_server_t, typename Server_session_t>
class Session_server_impl;

// Free functions.

/**
 * Prints string representation of the given `Server_session_impl` to the given `ostream`.
 *
 * @relatesalso Server_session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload,
         schema::ShmType S_SHM_TYPE_OR_NONE, size_t S_SHM_MAX_HNDL_SZ>
std::ostream& operator<<(std::ostream& os,
                         const Server_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES,
                                                   Mdt_payload, S_SHM_TYPE_OR_NONE, S_SHM_MAX_HNDL_SZ>& val);

/**
 * Prints string representation of the given `Client_session_impl` to the given `ostream`.
 *
 * @relatesalso Client_session_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<schema::MqType S_MQ_TYPE_OR_NONE, bool S_TRANSMIT_NATIVE_HANDLES, typename Mdt_payload,
         schema::ShmType S_SHM_TYPE_OR_NONE>
std::ostream& operator<<(std::ostream& os,
                         const Client_session_impl<S_MQ_TYPE_OR_NONE, S_TRANSMIT_NATIVE_HANDLES,
                                                   Mdt_payload, S_SHM_TYPE_OR_NONE>& val);

/**
 * Prints string representation of the given `Session_server_impl` to the given `ostream`.
 *
 * @relatesalso Session_server_impl
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Session_server_t, typename Server_session_t>
std::ostream& operator<<(std::ostream& os,
                         const Session_server_impl<Session_server_t, Server_session_t>& val);

} // namespace ipc::session
