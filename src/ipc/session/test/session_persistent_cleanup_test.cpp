/* Flow-IPC: Structured Transport
 * Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
 * Each commit is copyright by its respective author or author's employer.
 *
 * Licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */

/* Tests of kernel-persistent-resource (SHM pool names, MQ names, related) cleanup by the ipc::session
 * paradigm and the layers under it.  Two categories:
 *   - Civilized: a full session lifecycle (with channels) runs and ends gracefully; nothing named after the
 *     Server_app may remain in the dev-dirs (S_SHM_DEV_DIR, S_MQ_DEV_DIR).  (The CNS/PID file + mutex would
 *     remain by design; but the test_util.hpp session-pair machinery removes -- and asserts the existence of -- those.)
 *     Exercises, at once: SHM-classic session+app-scope pool removal; SHM-jemalloc arena-teardown pool
 *     removal (including the sweep in ~Owner_shm_pool_collection()); MQ + sentinel removal via the
 *     first-peer-dtor deleter (Blob_stream_mq_base_impl::ensure_unique_peer()).
 *   - Uncivilized (crash aftermath): the responsible party never ran its destructors; the *sweeps* must
 *     clean up later.  We test the sweep logic deterministically by *planting* conventionally-named
 *     corpses -- no actual crashing -- then triggering the sweeps by constructing the relevant
 *     ipc::session objects:
 *     - SHM-classic + MQs: swept synchronously in Session_server ctor (whole-app-prefix removal).
 *     - SHM-jemalloc: the asymmetric pair of sweeps -- Session_server-side (server-created-form names with
 *       *dead* PID server-namespace; deliberately skips client-created-form names) and Client_session-side
 *       (client-created-form names of its own Client_app with *dead* PID fragment).  Both are async
 *       (kicked at ctor onto low-priority workers) => poll-with-timeout; both also re-run periodically
 *       (30s), which we do not test -- the ctor-time kick runs the identical algorithm, and waiting out
 *       timers buys no new coverage.  (Technically that does skip some testing; here we are using white-box
 *       knowledge to rationalize that as of this writing it's fine; but later it might not be fine.  We just
 *       consider this acceptable for the time being.  @todo Revisit.)
 *     Dead PIDs are manufactured non-fakely: fork() a child that exits immediately; waitpid() it; use its PID. */

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/session/detail/session_shared_name.hpp"
#include "ipc/session/detail/shm/arena_lend/jemalloc/jemalloc_fwd.hpp"
#include "ipc/transport/detail/blob_stream_mq_impl.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/common.hpp"
#include "ipc/test/test_logger.hpp"
#include <gtest/gtest.h>
#include <flow/util/util.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <sys/wait.h>
#include <unistd.h>

namespace ipc::session::test
{

namespace
{

namespace fs = boost::filesystem;
using ipc::test::Test_logger;
using transport::struc::test::make_session_channel_pair;
using transport::Blob_stream_mq_base_impl;
using transport::Posix_mq_handle;
using transport::Bipc_mq_handle;
using session::schema::MqType;
using session::schema::ShmType;
using session::build_conventional_shared_name;
using util::Shared_name;
using util::process_id_t;
using std::string;

#ifndef FLOW_OS_LINUX
static_assert(false, "This test deals in Linux kernel-persistence specifics (the dev-dirs just below; fork(); "
                     "et al); check/adjust this file when porting.");
#endif

// Where the kernel-persistent objects appear as file names: SHM (pools, sentinels...) and POSIX MQs.
constexpr const char* S_SHM_DEV_DIR = "/dev/shm";
constexpr const char* S_MQ_DEV_DIR = "/dev/mqueue";

// Returns whether the kernel-persistent named object exists (dev_dir = a S_*_DEV_DIR).
bool name_exists(const char* dev_dir, const Shared_name& name)
{
  return fs::exists(fs::path{dev_dir} / name.str());
}

// Counts entries, in both dev-dirs, whose names contain the given substring.
size_t count_names_containing(const string& needle)
{
  size_t count = 0;
  for (const auto dev_dir : { S_SHM_DEV_DIR, S_MQ_DEV_DIR })
  {
    for (const auto& entry : fs::directory_iterator{fs::path{dev_dir}})
    {
      if (entry.path().filename().string().find(needle) != string::npos)
      {
        ++count;
      }
    }
  }
  return count;
}

// Polls until pred() or ~10s timeout; returns pred()'s final value.
template<typename Pred>
bool poll_until(const Pred& pred)
{
  for (int i = 0; i != 200; ++i)
  {
    if (pred())
    {
      return true;
    }
    flow::util::this_thread::sleep_for(boost::chrono::milliseconds(50));
  }
  return pred();
}

// Creates (and immediately closes handle to) a tiny SHM pool by that name; the name persists.
void plant_shm_pool(const Shared_name& name)
{
  bipc::shared_memory_object sho{util::CREATE_ONLY, name.native_str(),
                                 bipc::read_only};
  ASSERT_TRUE(name_exists(S_SHM_DEV_DIR, name));
}

void remove_planted_shm_pool(const Shared_name& name)
{
  EXPECT_TRUE(bipc::shared_memory_object::remove(name.native_str()));
}

// Returns the PID of a just-reaped (hence guaranteed-dead) child process.
process_id_t make_dead_pid()
{
  const auto pid = ::fork();
  if (pid == 0)
  {
    ::_exit(0);
  }
  int ignored;
  ::waitpid(pid, &ignored, 0);
  return process_id_t(pid);
}

/* Runs a full session-pair (with default init-channels) lifecycle and returns the Server_app/Client_app
 * names used; on return everything has been torn down. */
template<MqType MQ_TYPE, ShmType SHM_TYPE>
std::pair<string, string> run_session_lifecycle()
{
  auto pair = make_session_channel_pair<MQ_TYPE, true, SHM_TYPE>(nullptr);
  return { pair.m_sessions->m_srv_app.m_name, pair.m_sessions->m_cli_app.m_name };
}

/* The civilized-cleanup census: full lifecycle, then assert zero dev-dir residue naming the
 * Server_app.  (SHM-jemalloc teardown has asynchronous tail ends; poll-with-timeout.)  Note the
 * mission-critical assist from test_util.hpp: it removes the two by-design-persistent CNS items (PID file +
 * mutex) -- and *fails the test* if they did not exist -- so post-census-zero is exact, and CNS-artifact
 * existence is implicitly asserted in every session-based TEST in this whole suite. */
template<MqType MQ_TYPE, ShmType SHM_TYPE>
void census_after_civilized_lifecycle()
{
  const auto [srv_app_name, cli_app_name] = run_session_lifecycle<MQ_TYPE, SHM_TYPE>();
  EXPECT_TRUE(poll_until([&]() { return count_names_containing(srv_app_name) == 0; }))
    << "Kernel-persistent residue naming Server_app [" << srv_app_name << "] remained after graceful "
       "session teardown.";
}

/* The scope-binding census.  ipc::session's lifetime contract binds session-scope resources (each
 * session's SHM arena(s), its channels' MQs) to the Session objects' own lifetime; and app-scope
 * resources (the per-Client_app arena) to the Session_server's.  The SHM-providers themselves know
 * nothing of scopes; their (separately-tested) job is arena-dies => pools-vanish.  The *binding* is
 * what this asserts, observably, no white-boxing of which dtor calls what: count the dev-dir entries
 * naming the Server_app at 3 moments -- sessions live (peak); sessions destroyed, server alive (mid);
 * server destroyed (zero).  `mid < peak` proves session-scope items vanished on time (not lazily at
 * server end); `mid >= 2` proves an app-scope item correctly outlived the sessions (>= 2, not >= 1:
 * the by-design-persistent CNS mutex, which lives in the SHM dev-dir and names the Server_app,
 * legitimately accounts for exactly 1 mid-count entry). */
template<MqType MQ_TYPE, ShmType SHM_TYPE>
void census_scope_binding()
{
  auto raw_pair = make_session_channel_pair<MQ_TYPE, true, SHM_TYPE>(nullptr);
  auto& sessions = *raw_pair.m_sessions;
  const auto srv_app_name = sessions.m_srv_app.m_name;

  const auto peak = count_names_containing(srv_app_name);
  EXPECT_GT(peak, 0u);

  // Session-scope teardown -- channels first (they must predecease their sessions), then the
  // session pair; the Session_server lives on.
  raw_pair.m_cli_channels.clear();
  raw_pair.m_srv_channels.clear();
  sessions.destroy_sessions();

  size_t mid = 0;
  EXPECT_TRUE(poll_until([&]() // (The asynchronous-teardown-tails caveat applies here too.)
  {
    mid = count_names_containing(srv_app_name);
    return mid < peak;
  })) << "No session-scope kernel-persistent item disappeared upon session teardown "
         "(peak [" << peak << "]).";
  EXPECT_GE(mid, 2u)
    << "App-scope resources must outlive the sessions (they are Session_server-scoped); only the "
       "CNS mutex remained.";

  // Server teardown; the census must now reach exactly zero (post-CNS-item removal, as usual).
  sessions.m_srv.reset();
  sessions.remove_server_persistent_bits();
  EXPECT_TRUE(poll_until([&]() { return count_names_containing(srv_app_name) == 0; }))
    << "Residue naming Server_app [" << srv_app_name << "] remained after Session_server teardown.";
} // census_scope_binding()

} // Anonymous namespace

TEST(Session_persistent_cleanup_test, Civilized_census_heap)
{
  census_after_civilized_lifecycle<MqType::NONE, ShmType::NONE>();
}

TEST(Session_persistent_cleanup_test, Civilized_census_posix_mq_shm_classic)
{
  census_after_civilized_lifecycle<MqType::POSIX, ShmType::CLASSIC>();
}

TEST(Session_persistent_cleanup_test, Civilized_census_bipc_mq_shm_jemalloc)
{
  census_after_civilized_lifecycle<MqType::BIPC, ShmType::JEMALLOC>();
}

// (No heap variant for these 2: with no SHM there is nothing app-scoped to outlive the sessions.)
TEST(Session_persistent_cleanup_test, Scope_binding_census_posix_mq_shm_classic)
{
  census_scope_binding<MqType::POSIX, ShmType::CLASSIC>();
}

TEST(Session_persistent_cleanup_test, Scope_binding_census_bipc_mq_shm_jemalloc)
{
  census_scope_binding<MqType::BIPC, ShmType::JEMALLOC>();
}

/* SHM-classic crash-sweep: plant a pool-name corpse under the Server_app's conventional prefix (with a
 * bogus server-namespace: the sweep removes everything under the app prefix, PID-liveness not relevant);
 * constructing the next classic Session_server (inside the pair factory) sweeps it, synchronously in ctor. */
TEST(Session_persistent_cleanup_test, Crash_sweep_shm_classic)
{
  // Lifecycle #1 just to learn the app names for this combo (and prove clean baseline).
  const auto [srv_app_name, cli_app_name] = run_session_lifecycle<MqType::NONE, ShmType::CLASSIC>();

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

/* MQ crash-sweep (all ipc::session variants inherit this, heap-backed included): plant an MQ corpse + its
 * two sentinel SHM-pools under the app's conventional prefix; the vanilla Session_server ctor sweep is
 * keyed on the MQ listing and removes MQ + sentinels together (Blob_stream_mq_base::remove_persistent()). */
template<MqType MQ_TYPE, typename Mq>
void crash_sweep_mq()
{
  using Mq_base_impl = Blob_stream_mq_base_impl<Mq>;

  const auto [srv_app_name, cli_app_name] = run_session_lifecycle<MQ_TYPE, ShmType::NONE>();

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

  const auto [srv_app_name, cli_app_name] = run_session_lifecycle<MqType::NONE, ShmType::JEMALLOC>();

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
