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

namespace ipc::session
{

// Types.

/**
 * This is the data-less sub-class of Server_session or any more-advanced (e.g., SHM-capable) variant thereof
 * that exposes `protected` APIs hidden from public user by providing public access to them; this is used internally
 * by Session_server.  The background is briefly explained in the impl section of Server_session doc header.
 *
 * @tparam Server_session_t
 *         The object whose `protected` stuff to expose.
 */
template<typename Server_session_t>
class Server_session_dtl :
  public Server_session_t
{
public:
  // Types.

  /// Short-hand for base class.
  using Base = Server_session_t;

  /// See `protected` counterpart.
  using Session_base_obj = typename Base::Session_base_obj;

  // Constructors/destructor.

  /**
   * See `protected` counterpart.
   *
   * @param logger_ptr
   *        See `protected` counterpart.
   * @param srv_app_ref
   *        See `protected` counterpart.
   * @param master_channel_sock_stm
   *        See `protected` counterpart.
   */
  explicit Server_session_dtl(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                              transport::sync_io::Native_socket_stream&& master_channel_sock_stm);

  // Methods.

  /**
   * See `protected` counterpart.
   *
   * @param srv
   *        See `protected` counterpart.
   * @param init_channels_by_srv_req
   *        See `protected` counterpart.
   * @param mdt_from_cli_or_null
   *        See `protected` counterpart.
   * @param init_channels_by_cli_req
   *        See `protected` counterpart.
   * @param cli_app_lookup_func
   *        See `protected` counterpart.
   * @param cli_namespace_func
   *        See `protected` counterpart.
   * @param pre_rsp_setup_func
   *        See `protected` counterpart.
   * @param n_init_channels_by_srv_req_func
   *        See `protected` counterpart.
   * @param mdt_load_func
   *        See `protected` counterpart.
   * @param on_done_func
   *        See `protected` counterpart.
   */
  template<typename Session_server_impl_t,
           typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
           typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
  void async_accept_log_in(Session_server_impl_t* srv,
                           typename Base::Channels* init_channels_by_srv_req,
                           typename Base::Mdt_reader_ptr* mdt_from_cli_or_null,
                           typename Base::Channels* init_channels_by_cli_req,
                           Cli_app_lookup_func&& cli_app_lookup_func, Cli_namespace_func&& cli_namespace_func,
                           Pre_rsp_setup_func&& pre_rsp_setup_func,
                           N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
                           Mdt_load_func&& mdt_load_func,
                           Task_err&& on_done_func);

  /**
   * Provides `const` access to Session_base super-object.
   * @return See above.
   */
  const Session_base_obj& base() const;
}; // class Server_session_dtl

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_SRV_SESSION_DTL \
  template<typename Server_session_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_SRV_SESSION_DTL \
  Server_session_dtl<Server_session_t>

TEMPLATE_SRV_SESSION_DTL
CLASS_SRV_SESSION_DTL::Server_session_dtl(flow::log::Logger* logger_ptr, const Server_app& srv_app_ref,
                                          transport::sync_io::Native_socket_stream&& master_channel_sock_stm) :
  Base(logger_ptr, srv_app_ref, std::move(master_channel_sock_stm))
{
  // Yep.
}

TEMPLATE_SRV_SESSION_DTL
template<typename Session_server_impl_t,
         typename Task_err, typename Cli_app_lookup_func, typename Cli_namespace_func, typename Pre_rsp_setup_func,
         typename N_init_channels_by_srv_req_func, typename Mdt_load_func>
void CLASS_SRV_SESSION_DTL::async_accept_log_in
       (Session_server_impl_t* srv,
        typename Base::Channels* init_channels_by_srv_req,
        typename Base::Mdt_reader_ptr* mdt_from_cli_or_null,
        typename Base::Channels* init_channels_by_cli_req,
        Cli_app_lookup_func&& cli_app_lookup_func,
        Cli_namespace_func&& cli_namespace_func,
        Pre_rsp_setup_func&& pre_rsp_setup_func,
        N_init_channels_by_srv_req_func&& n_init_channels_by_srv_req_func,
        Mdt_load_func&& mdt_load_func,
        Task_err&& on_done_func)
{
  Base::async_accept_log_in(srv, init_channels_by_srv_req, mdt_from_cli_or_null, init_channels_by_cli_req,
                            std::move(cli_app_lookup_func), std::move(cli_namespace_func),
                            std::move(pre_rsp_setup_func),
                            std::move(n_init_channels_by_srv_req_func), std::move(mdt_load_func),
                            std::move(on_done_func));
}

TEMPLATE_SRV_SESSION_DTL
const typename CLASS_SRV_SESSION_DTL::Session_base_obj& CLASS_SRV_SESSION_DTL::base() const
{
  return Base::base();
}

#undef CLASS_SRV_SESSION_DTL
#undef TEMPLATE_SRV_SESSION_DTL

} // namespace ipc::session
