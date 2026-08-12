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

#include "ipc/session/test/session_persistent_cleanup_test.hpp"

namespace ipc::session::test
{

// A slice of the test battery (the ShmType-JEMALLOC tests); see similarly named .hpp.

TEST(Session_persistent_cleanup_test, Civilized_census_bipc_mq_shm_jemalloc)
{
  census_after_civilized_lifecycle<MqType::BIPC, ShmType::JEMALLOC>();
}

// (No heap variant: with no SHM there is nothing app-scoped to outlive the sessions.)
TEST(Session_persistent_cleanup_test, Scope_binding_census_bipc_mq_shm_jemalloc)
{
  census_scope_binding<MqType::BIPC, ShmType::JEMALLOC>();
}

/* SHM-jemalloc crash-sweeps: the asymmetric pair, plus negative controls.  Name shapes (see
 * session::shm::arena_lend::jemalloc session_shared_name and the sweeps in [Client_session|Session_server]
 * impls): server-created pools = <session-scope prefix>/jem/<pool-id>; client-created pools add the creating
 * client process's PID: <session-scope prefix>/jem/<pid>/<pool-id>.
 *   - Server-side sweep (async, ctor-kicked): removes server-form names whose server-namespace PID is dead;
 *     deliberately skips client-form names (not its property to delete).
 *   - Client-side sweep (async, ctor-kicked): removes client-form names of its *own* Client_app whose
 *     PID-fragment is dead; any server-app/namespace.
 * Plants: [A] server-form, dead PID => swept by server-side.  [B] client-form (our Client_app), dead PID
 * fragment => skipped by server-side, swept by client-side.  [C] server-form, *live* (our) PID => survives
 * (liveness guard).  [D] client-form, live PID fragment => survives (ditto).  [E] client-form, dead PID but
 * a *foreign* Client_app => survives both (server-side: wrong form; client-side: wrong app) -- the
 * asymmetry made visible.  C, D, E are removed manually at the end. */
TEST(Session_persistent_cleanup_test, Crash_sweep_shm_jemalloc)
{
  using session::shm::arena_lend::jemalloc::SHM_SUBTYPE_PREFIX;

  const auto app_names = run_session_lifecycle<MqType::NONE, ShmType::JEMALLOC>();
  const auto& srv_app_name = app_names.first;
  const auto& cli_app_name = app_names.second;

  const auto dead_pid = make_dead_pid();
  const auto live_pid = util::Process_credentials::own_process_id();

  const auto make_name = [&](pid_t srv_ns_pid, bool client_form, pid_t fragment_pid, const string& cli_app)
  {
    auto name = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                               Shared_name::ct(srv_app_name),
                                               Shared_name::ct_from_int(srv_ns_pid),
                                               Shared_name::ct(cli_app),
                                               Shared_name::ct("7"))
                  / SHM_SUBTYPE_PREFIX;
    if (client_form)
    {
      name /= Shared_name::ct_from_int(fragment_pid);
    }
    return name / Shared_name::ct("424242"); // "Pool ID."
  };

  const auto corpse_a = make_name(dead_pid, false, 0, cli_app_name);
  const auto corpse_b = make_name(dead_pid, true, dead_pid, cli_app_name);
  const auto corpse_c = make_name(live_pid, false, 0, cli_app_name);
  const auto corpse_d = make_name(dead_pid, true, live_pid, cli_app_name);
  const auto corpse_e = make_name(dead_pid, true, dead_pid, cli_app_name + "fake");
  for (const auto& corpse : { corpse_a, corpse_b, corpse_c, corpse_d, corpse_e })
  {
    plant_shm_pool(corpse);
  }

  {
    auto pair = make_session_channel_pair<MqType::NONE, true, ShmType::JEMALLOC>(nullptr);

    // The sweeps are ctor-kicked but async (worker threads): poll.
    EXPECT_TRUE(poll_until([&]() { return !name_exists(S_SHM_DEV_DIR, corpse_a); }))
      << "Server-side sweep failed to remove dead-PID server-form corpse.";
    EXPECT_TRUE(poll_until([&]() { return !name_exists(S_SHM_DEV_DIR, corpse_b); }))
      << "Client-side sweep failed to remove dead-PID client-form corpse.";

    // Negative controls (the sweeps above have demonstrably run by now).
    EXPECT_TRUE(name_exists(S_SHM_DEV_DIR, corpse_c)) << "Live-PID server-form was wrongly removed.";
    EXPECT_TRUE(name_exists(S_SHM_DEV_DIR, corpse_d)) << "Live-PID-fragment client-form was wrongly removed.";
    EXPECT_TRUE(name_exists(S_SHM_DEV_DIR, corpse_e)) << "Foreign-Client_app client-form was wrongly removed "
                                                      "(server-side sweep must skip client-form; client-side "
                                                      "must skip foreign apps).";
  }

  for (const auto& corpse : { corpse_c, corpse_d, corpse_e })
  {
    remove_planted_shm_pool(corpse);
  }
}

} // namespace ipc::session::test
