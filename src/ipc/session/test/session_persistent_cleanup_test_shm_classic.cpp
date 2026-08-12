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

// A slice of the test battery (the ShmType-CLASSIC tests); see similarly named .hpp.

TEST(Session_persistent_cleanup_test, Civilized_census_posix_mq_shm_classic)
{
  census_after_civilized_lifecycle<MqType::POSIX, ShmType::CLASSIC>();
}

// (No heap variant: with no SHM there is nothing app-scoped to outlive the sessions.)
TEST(Session_persistent_cleanup_test, Scope_binding_census_posix_mq_shm_classic)
{
  census_scope_binding<MqType::POSIX, ShmType::CLASSIC>();
}

/* SHM-classic crash-sweep: plant a pool-name corpse under the Server_app's conventional prefix (with a
 * bogus server-namespace: the sweep removes everything under the app prefix, PID-liveness not relevant);
 * constructing the next classic Session_server (inside the pair factory) sweeps it, synchronously in ctor. */
TEST(Session_persistent_cleanup_test, Crash_sweep_shm_classic)
{
  // Lifecycle #1 just to learn the app names for this combo (and prove clean baseline).
  const auto app_names = run_session_lifecycle<MqType::NONE, ShmType::CLASSIC>();
  const auto& srv_app_name = app_names.first;
  const auto& cli_app_name = app_names.second;

  const auto corpse
    = build_conventional_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM,
                                     Shared_name::ct(srv_app_name),
                                     Shared_name::ct("999999999"), // Bogus long-dead "server-namespace."
                                     Shared_name::ct(cli_app_name),
                                     Shared_name::ct("1"))
        / Shared_name::ct("fakeCorpsePool1");
  plant_shm_pool(corpse);

  {
    auto pair = make_session_channel_pair<MqType::NONE, true, ShmType::CLASSIC>(nullptr);
    EXPECT_FALSE(name_exists(S_SHM_DEV_DIR, corpse)) << "Classic Session_server ctor sweep missed the corpse.";
  }
}

} // namespace ipc::session::test
