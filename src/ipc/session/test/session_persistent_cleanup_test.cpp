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

// A slice of the test battery (the no-SHM tests: heap census, MQ sweeps, App-name contract); see
// similarly named .hpp.

TEST(Session_persistent_cleanup_test, Civilized_census_heap)
{
  census_after_civilized_lifecycle<MqType::NONE, ShmType::NONE>();
}

/* MQ crash-sweep (all ipc::session variants inherit this, heap-backed included): plant an MQ corpse + its
 * two sentinel SHM-pools under the app's conventional prefix; the vanilla Session_server ctor sweep is
 * keyed on the MQ listing and removes MQ + sentinels together (Blob_stream_mq_base::remove_persistent()). */
template<MqType MQ_TYPE, typename Mq>
void crash_sweep_mq()
{
  using Mq_base_impl = Blob_stream_mq_base_impl<Mq>;

  const auto app_names = run_session_lifecycle<MQ_TYPE, ShmType::NONE>();
  const auto& srv_app_name = app_names.first;
  const auto& cli_app_name = app_names.second;

  const auto mq_corpse
    = build_conventional_shared_name(Mq::S_RESOURCE_TYPE_ID,
                                     Shared_name::ct(srv_app_name),
                                     Shared_name::ct("999999999"),
                                     Shared_name::ct(cli_app_name),
                                     Shared_name::ct("1"))
        / Shared_name::ct("fakeCorpseMq1");
  const auto sentinel_corpse_1 = Mq_base_impl::mq_sentinel_name(mq_corpse, true);
  const auto sentinel_corpse_2 = Mq_base_impl::mq_sentinel_name(mq_corpse, false);

  {
    Test_logger logger{flow::log::Sev::S_WARNING};
    Mq mq{&logger, mq_corpse, util::CREATE_ONLY, 1, 8}; // Handle closes at scope end; the MQ name persists.
  }
  plant_shm_pool(sentinel_corpse_1);
  plant_shm_pool(sentinel_corpse_2);

  {
    auto pair = make_session_channel_pair<MQ_TYPE, true, ShmType::NONE>(nullptr);
    EXPECT_FALSE(name_exists(std::is_same_v<Mq, Posix_mq_handle> ? S_MQ_DEV_DIR : S_SHM_DEV_DIR, mq_corpse))
      << "Session_server ctor MQ sweep missed the MQ corpse.";
    EXPECT_FALSE(name_exists(S_SHM_DEV_DIR, sentinel_corpse_1)) << "MQ sweep missed sentinel 1.";
    EXPECT_FALSE(name_exists(S_SHM_DEV_DIR, sentinel_corpse_2)) << "MQ sweep missed sentinel 2.";
  }
}

TEST(Session_persistent_cleanup_test, Crash_sweep_posix_mq)
{
  crash_sweep_mq<MqType::POSIX, Posix_mq_handle>();
}

TEST(Session_persistent_cleanup_test, Crash_sweep_bipc_mq)
{
  crash_sweep_mq<MqType::BIPC, Bipc_mq_handle>();
}

/* The App-name contract (App::m_name doc header: non-empty, no util::Shared_name::S_SEPARATOR) -- on which
 * the conventional naming scheme, hence notably the sweeps above, depends -- is enforced at Session_server
 * ctor (all variants funnel into the same validation).  Assert the enforcement: bad server name; then good
 * server name but bad (registered) client name. */
TEST(Session_persistent_cleanup_test, App_name_contract_enforced)
{
  Error_code creds_err;
  const auto self_exe
    = util::Process_credentials::own_process_credentials().process_invoked_as(&creds_err);
  ASSERT_FALSE(creds_err);
  const fs::path work_dir = fs::canonical(fs::current_path().lexically_normal());
  const auto uid = ::geteuid();
  const auto gid = ::getegid();

  const auto expect_invalid = [&](const string& srv_name, const string& cli_name)
  {
    session::Client_app cli_app{{ cli_name, self_exe, uid, gid }};
    const session::Client_app::Master_set cli_apps{{ cli_app.m_name, cli_app }};
    session::Server_app srv_app{{ srv_name, self_exe, uid, gid },
                                { cli_app.m_name }, work_dir, util::Permissions_level::S_USER_ACCESS};
    Error_code err_code;
    session::Session_server<MqType::NONE, false> srv{nullptr, srv_app, cli_apps, &err_code};
    EXPECT_EQ(err_code, session::error::Code::S_INVALID_ARGUMENT)
      << "srv-name [" << srv_name << "] cli-name [" << cli_name << "].";
  };

  expect_invalid("cleanupTest_badSrv", "cleanupTestCli"); // Separator in server name.
  expect_invalid("cleanupTestSrv", "cleanupTest_badCli"); // Separator in a client name.
  expect_invalid("", "cleanupTestCli"); // Empty server name.
}

} // namespace ipc::session::test
