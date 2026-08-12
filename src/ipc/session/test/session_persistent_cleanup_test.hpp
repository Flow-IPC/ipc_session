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

#pragma once

/* Test battery: the shared substance of the similarly named *.cpp files (siblings) -- not an API header.
 * The helpers below are function templates over certain <knobs> (MqType x ShmType); the sibling .cpp s
 * spread the TEST()s expanding them across translation units.  This bounds compiler RAM use per .cpp
 * (translation unit) versus simply placing everything into one .cpp.  In particular each per-<knobs>
 * ipc::session stack costs GBs of compiler RAM, for debug-info builds, at least with some Linux gcc.
 * 2+ such translation units compiling concurrently can exhaust a smaller build machine (including
 * GitHub CI runners in 2026). */

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
// (`inline` on the plain helpers here and below: a sibling TU may use only a subset; -Wunused-function otherwise.)
inline bool name_exists(const char* dev_dir, const Shared_name& name)
{
  return fs::exists(fs::path{dev_dir} / name.str());
}

// Counts entries, in both dev-dirs, whose names contain the given substring.
inline size_t count_names_containing(const string& needle)
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
inline void plant_shm_pool(const Shared_name& name)
{
  bipc::shared_memory_object sho{util::CREATE_ONLY, name.native_str(),
                                 bipc::read_only};
  ASSERT_TRUE(name_exists(S_SHM_DEV_DIR, name));
}

inline void remove_planted_shm_pool(const Shared_name& name)
{
  EXPECT_TRUE(bipc::shared_memory_object::remove(name.native_str()));
}

// Returns the PID of a just-reaped (hence guaranteed-dead) child process.
inline process_id_t make_dead_pid()
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
  const auto& srv_app_name = run_session_lifecycle<MQ_TYPE, SHM_TYPE>().first;
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

} // namespace ipc::session::test
