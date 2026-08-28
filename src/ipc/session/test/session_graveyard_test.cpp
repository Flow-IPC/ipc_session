/* Flow-IPC: Sessions
 * Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
 * Each commit is copyright by its respective author or author's employer.
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

/* Session_server incomplete-session graveyard test.  Scenario: the low-level (socket) accept succeeds, but the
 * subsequent session log-in fails (we connect a raw Native_socket_stream and slam it shut) -- so the server's
 * incomplete Server_session must be destroyed; doing so in its own handler thread would self-deadlock, hence
 * Session_server hands it to the graveyard thread whose sole job is running such dtors in peace.  We verify the
 * burial actually happens (not merely gets queued): the failed session's resources (threads' FDs, socket FDs,
 * timer FDs) are freed -- open-FD census returns to baseline -- while the server lives on and can still accept
 * a well-behaved session afterward.  Exercised with both Session_server ctor forms; the throwing form is the
 * one whose graveyard was, prior to the associated bug fix, never started at all.
 *
 * @todo Ideally there should also be a per-SHM-provider version of this test (1 for SHM-classic, 1 for SHM-jemalloc
 * as of this writing) -- done without copy/pasting, as we do with most session-related tests.  White-boxily
 * speaking, it's probably not totally necessary; the Session_server sub-classes will cleanly not concern themselves
 * with this thing in the Session_server super-class.  Still, it's peace of mind, and it's a fast-running test
 * and just would need a couple more short test source files. */

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/session/detail/session_shared_name.hpp"
#include "ipc/transport/native_socket_stream.hpp"
#include "ipc/transport/native_socket_stream_acceptor.hpp"
#include <flow/common.hpp>
#include <boost/thread/future.hpp>
#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>

namespace ipc::session::test
{

namespace
{

namespace fs = boost::filesystem;
using flow::Error_code;
using boost::chrono::milliseconds;
using boost::chrono::seconds;

// Vanilla (heap; no MQs, no handles pipe) session types: the graveyard machinery is identical across configs.
using Pair = transport::struc::test::Session_pair<schema::MqType::NONE, false, schema::ShmType::NONE>;

constexpr auto TIMEOUT = seconds{5};

#ifndef FLOW_OS_LINUX
static_assert(false, "Not tested in non-Linux (/proc/self/fd census below); look into it when porting.");
#endif
// Count of open FDs in this process.  (Includes the directory-iteration's own FD -- consistent, so deltas work.)
size_t n_open_fds()
{
  size_t n = 0;
  for ([[maybe_unused]] const auto& entry : fs::directory_iterator{"/proc/self/fd"})
  {
    ++n;
  }
  return n;
}

/* The absolute Shared_name at which m_srv_app's running Session_server accepts master-channel socket connects.
 * Mirrors Session_base::session_master_socket_stream_acceptor_absolute_name() -- same building blocks -- with the
 * srv-namespace read from the CNS (PID) file, exactly the way a real Client_session learns it. */
util::Shared_name master_acceptor_name(Pair& pair)
{
  std::ifstream cns_file{pair.cns_path().string()};
  std::string srv_namespace;
  cns_file >> srv_namespace;
  EXPECT_FALSE(srv_namespace.empty()) << "CNS (PID) file [" << pair.cns_path() << "] missing or empty; "
                                         "Session_server should have written it at construction.";

  auto acc_name
    = build_conventional_shared_name(transport::Native_socket_stream_acceptor::S_RESOURCE_TYPE_ID,
                                     util::Shared_name::ct(pair.m_srv_app.m_name),
                                     util::Shared_name::ct(srv_namespace));
  acc_name /= util::Shared_name::S_SENTINEL;
  return acc_name;
}

/* One full scenario against a fresh Session_server built by `make_server` (which uses either ctor form):
 * botched log-in => burial => FD census back to baseline => server still accepts a good session. */
void graveyard_scenario(Pair& pair, const std::function<std::unique_ptr<Pair::Session_server_t> ()>& make_server)
{
  namespace this_thread = flow::util::this_thread;

  pair.m_srv = make_server();

  /* Attn: baseline *before* arming the accept: async_accept() pre-creates the incomplete Server_session --
   * threads, loops, FDs and all -- at request time.  Post-burial (request consumed, nothing re-armed) the
   * process returns to *this* resting state; an after-arming baseline would sit ~a-session's-worth of FDs
   * too high, and the equality poll below would never hit. */
  const size_t baseline_fds = n_open_fds();

  // Arm an accept; it should complete -- with a truthy Error_code -- once we botch a log-in below.
  typename Pair::Server_session_t srv_session;
  Error_code accept_err;
  boost::promise<void> accept_done;
  pair.m_srv->async_accept(&srv_session,
                           [&](const Error_code& err_code) { accept_err = err_code; accept_done.set_value(); });

  { // Raw connect straight to the master acceptor; then slam shut without speaking any log-in.
    Error_code err_code;
    transport::Native_socket_stream raw_peer{nullptr, "graveyard_raw"};
    raw_peer.sync_connect(master_acceptor_name(pair), &err_code);
    EXPECT_FALSE(err_code) << "Raw connect to master acceptor failed: [" << err_code << "] ["
                           << err_code.message() << "].";
  } // raw_peer closes here; server-side incomplete Server_session's log-in shall fail (EOF).

  // The accept handler must fire, with a truthy code (log-in failure); the incomplete session goes to the graveyard.
  auto accept_fut = accept_done.get_future();
  ASSERT_NE(accept_fut.wait_for(TIMEOUT), boost::future_status::timeout) << "Accept handler never fired.";
  accept_fut.get();
  EXPECT_TRUE(accept_err) << "Accept unexpectedly succeeded despite botched log-in.";
  std::cout << "Botched log-in reported to accept handler: [" << accept_err << "] [" << accept_err.message()
            << "].\n" << std::flush;

  /* The burial is the part under test: ~Server_session must actually run (in the graveyard thread), freeing the
   * failed session's FDs -- as opposed to sitting forever in a never-started loop's queue (the bug this test
   * guards against, in the throwing-ctor-form case).  Bounded-poll the FD census back down to baseline. */
  size_t fds_now = 0;
  for (unsigned int n_polls_left = 500; n_polls_left != 0; --n_polls_left)
  {
    fds_now = n_open_fds();
    if (fds_now == baseline_fds)
    {
      break;
    }
    this_thread::sleep_for(milliseconds{10});
  }
  EXPECT_EQ(fds_now, baseline_fds)
    << "Open-FD census did not return to baseline: the incomplete Server_session was seemingly never destroyed.  "
       "Graveyard thread not running (or not reached)?";
  if (fds_now == baseline_fds)
  {
    std::cout << "FD census back to baseline [" << baseline_fds << "]: burial confirmed.\n" << std::flush;
  }

  // The server must remain healthy: a well-behaved session pair connects fine.  (Consumes the armed state anew.)
  pair.connect_sessions(nullptr);
  pair.destroy_sessions();

  pair.m_srv.reset(); // Next scenario (if any) builds its own server.
}

} // namespace (anon)

TEST(Session_graveyard_test, failed_log_in_reaped)
{
  Pair pair;
  pair.populate_apps("Gy");
  pair.remove_server_persistent_bits(false); // Pre-clean possible leavings of a previous run.

  const auto& srv_app = pair.m_srv_apps.find(pair.m_srv_app.m_name)->second;

  std::cout << "Scenario 1: Session_server error-code ctor form.\n" << std::flush;
  graveyard_scenario(pair, [&]()
  {
    Error_code err_code;
    auto srv = std::make_unique<Pair::Session_server_t>(nullptr, srv_app, pair.m_cli_apps, &err_code);
    EXPECT_FALSE(err_code) << "Session_server ctor failed: [" << err_code << "] [" << err_code.message() << "].";
    return srv;
  });

  std::cout << "Scenario 2: Session_server throwing ctor form (the one whose graveyard the fixed bug disabled).\n"
            << std::flush;
  graveyard_scenario(pair, [&]()
  {
    return std::make_unique<Pair::Session_server_t>(nullptr, srv_app, pair.m_cli_apps);
  });

  // Both scenarios reset m_srv; hence ~Session_pair would skip persistent-bits cleanup.  Do it here.
  pair.remove_server_persistent_bits();
}

} // namespace ipc::session::test
