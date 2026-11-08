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
 * The tests below are type-parameterized (TYPED_TEST_P) on the session type under test; each sibling .cpp
 * instantiates the registered suite for its one type; together they cover the full matrix.
 * This bounds compiler RAM use per .cpp (translation unit) versus simply placing everything into one .cpp.
 * In particular each per-type ipc::session stack costs GBs of compiler RAM, for debug-info
 * builds, at least with some Linux gcc.  2+ such translation units compiling concurrently can exhaust a
 * smaller build machine (including GitHub CI runners in 2026). */

/* Unit tests of ipc::session session-establishment (and establishment-failure) behavior: the connect/accept
 * machinery itself, as opposed to what an established session can do (channels, SHM, ...) -- other tests
 * (and the transport_test exercise-mode integration test) cover the latter.  The tests here focus on
 * corner/failure paths that integration testing does not reach; the happy path is exercised ubiquitously
 * (elsewhere and incidentally by this file's own scaffolding).
 *
 * What is covered here so far:
 *   - Connect attempted when no server has ever run: there is no CNS (PID file) to read.  The client must
 *     fail gracefully with the OS's file-not-found error; and the same Client_session object must then be
 *     usable to connect successfully, once a server does run.
 *   - Connect against a corrupt CNS (PID file): each of the two malformed-contents paths (no
 *     newline-terminated first line; line is not a number) must yield CLIENT_NAMESPACE_STORE_BAD_FORMAT.
 *   - The stale-CNS scenario: a server ran and is gone; the CNS file remains (in production nothing deletes
 *     it -- on purpose -- the next server instance overwrites it in place); a client connect therefore
 *     reads the CNS fine but fails to reach the (dead) server's socket-acceptor.  Then a new server comes
 *     up, and the *same* Client_session object retries and must succeed.  This is a regression test:
 *     the failed attempt's internal rollback must fully return the object to its initial state (namely
 *     clear the namespaces gleaned from the CNS read) for the retry to be possible.
 *   - Identity/authorization rejects at log-in: unknown Client_app; known-but-disallowed Client_app;
 *     allowed Client_app but registered (server-side) with a wrong UID or a wrong executable path.
 *     The server must emit the specific applicable error to its accept handler, tell the rejected
 *     client nothing specific (by design), and remain fully operational for a subsequent proper client.
 *   - The server's own identity self-check at construction: a Server_app with a wrong UID, or a wrong (or
 *     merely differently spelled) executable path, must fail the Session_server ctor with the specific code --
 *     via out-arg, or exception in the throwing form; the failed object's dtor must be harmless.
 *   - Compile-time session-config mismatch between the two sides (e.g., differing MQ-type template
 *     parameter): rejected similarly to the above.
 *   - The almost-PEER state of a freshly-accepted Server_session (init_handlers() not yet called):
 *     the documented API no-ops/sentinels are in force; everything comes alive after init_handlers().
 *   - NULL state (default-cted or moved-from session objects, both sides): the Session-concept sentinels; plus
 *     get_logger() null and get_log_component() usable, with no impl inside.
 *   - Channel passive-open rejection: a peer constructed without a passive-open handler causes the
 *     opposing side's active open_channel() to emit the specific non-fatal error; the session survives.
 *   - Crossing active-opens (regression test): both sides open_channel() repeatedly and concurrently,
 *     each passive-accepted by the other; every open must succeed promptly (rather than the two sides
 *     stalling each other until the internal timeout), and every passive side must see every channel.
 *   - Opposing user closes a just-passive-opened channel immediately -- in each direction (server user closes,
 *     client opens; and mirrored): the active open must either succeed or fail with the non-fatal system error,
 *     never abort; the session survives.
 *   - A pending async_accept() aborted by Session_server destruction: its handler must fire, with the
 *     specific object-shutdown code.
 *   - Session_server destroyed while session-opens are in flight (many rounds, varied timing, surplus clients):
 *     each accept handler fires exactly once, with success or the object-shutdown code; every client connect
 *     returns; emitted sessions outlive the server.  (ASAN/TSAN CI runs are the main detectors.)  Each round also
 *     constructs a new Session_server for the same Server_app after the previous one's sessions are all gone.
 *   - A used session (struc::Channel traffic over SHM where applicable) ends -- client side first, then, at varied
 *     times, the server side -- while another session-open for the same app is in flight against the same,
 *     still-listening Session_server: the latter succeeds, and its sessions (including the app-scope arena) work.
 *   - Two async_accept()s outstanding concurrently, satisfied by two clients: both complete; the two
 *     resulting sessions coexist and are correctly paired.
 *   - The `sync_io`-pattern server-side adapters' API-misuse guards (Session_server_adapter::async_accept(),
 *     Server_session_adapter::init_handlers()): each misuse -- before start_ops(); on a not-yet-accepted session;
 *     duplicate; after the session error -- is refused with no residue, and the proper sequence then works.
 *   - Session_server::mq_msg_size_limit() is per session, fixed at async_accept() time: a later change does not
 *     affect channels an earlier-accepted session opens afterwards.
 *   - async_accept() targeting a PEER-state session empties it synchronously at the call; a new client then
 *     connects into that same object.
 *   - A PEER-state Server_session outliving its Session_server keeps working: it opens a channel; SHM-backed, its
 *     app-scope arena still round-trips a lend/borrow.
 *   - The graceful session-end choreography, in each direction (server-side session destroyed first;
 *     client-side first): the observing side's error handler fires exactly once -- for SHM-jemalloc
 *     with the specific SESSION_FINISHED code -- while the initiating side's handler never fires; the
 *     hosed session's APIs behave per contract (no-ops/sentinels); and, SHM-jemalloc only, the
 *     initiator's dtor demonstrably blocks until the observer's dtor begins (the Graceful_finisher
 *     gate) -- versus completing promptly for the other session types.  (Kernel-persistent leftovers
 *     of a graceful end are session_persistent_cleanup_test.cpp's department, not ours.)
 *   - The Session_server run-time config knobs: mq_msg_size_limit() get/set/read-back (including its
 *     round-up-to-alignment-multiple behavior) on all 3 server types; pool_size_limit_mi() ditto on
 *     the SHM-classic one; and that a server with adjusted knobs still vends working sessions.
 *   - The SHM accessor shape of each session type: which of session_shm()/app_shm()/shm_session()
 *     exist per type and side (including compile-time absences: e.g., SHM-jemalloc
 *     Client_session has no app_shm() -- deliberately); that they return non-null in PEER state; that
 *     the server-level app_shm(Client_app) agrees with the per-session accessor; and, SHM-classic,
 *     that the Session-level lend_object()/borrow_object() wrappers correctly route both
 *     session-scope and app-scope objects (the scope is encoded in the lend blob).
 *   - (In the separate Session_channel_matrix_test suite -- session_connect_test.cpp:) channel
 *     establishment across the 6 channel/transport configs -- see the section comment there.
 *
 * Each test runs 3x: on vanilla sessions, SHM-classic-backed sessions, and SHM-jemalloc-backed sessions.
 * The channel/transport config is (tied for) the most-full-featured one (POSIX MQs + native-handles transport);
 * per the master testing plan other configs get their due in a dedicated channel-establishment matrix test.
 *
 * We use the struc::test::Session_pair utility (test_util.hpp) for the boring parts (App universe,
 * teardown ordering, kernel-persistent cleanup) but drive the actual Session_server/Client_session
 * objects directly: connecting/accepting is the subject here, not scaffolding. */

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include "ipc/transport/struc/channel_base.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/session/error.hpp"
#include "ipc/session/sync_io/session_server_adapter.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/test/test_logger.hpp"
#include "ipc/test/test_shm_util.hpp"
#include <flow/test/test_common_util.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <memory>
#include <type_traits>
#include <vector>

namespace ipc::session::test
{

namespace
{

using transport::struc::test::Session_pair;
using ipc::test::Test_logger;
using flow::log::Sev;
using std::string;

/* Session cfg per the master plan: POSIX MQs + native-handles transport = the most-full-featured config.
 * (The SMC -- session master channel, used internally by the machinery under test -- is always atop a
 * socket-stream channel regardless of this, so this knob concerns user channels only; still, it is part
 * of the log-in handshake and thus worth running in the standard config.) */
template<schema::ShmType S_SHM_TYPE>
using Cfg_session_pair = Session_pair<schema::MqType::POSIX, true, S_SHM_TYPE>;

/* Compile-time probes: does the given Session (or, for the `Has_server_*` ones, Session_server) type
 * expose the given SHM-related API?  The presence or absence of each, per SHM-provider type and side,
 * is part of the API's deliberate shape and is asserted by the Shm_accessors test.  Besides
 * shape-drift protection there is a subtler point: these members are templates, so a nonsensical impl
 * of a never-called one does not even fail compilation; the asserts here at least verify the intended
 * roster, while serializer_stats_test.cpp instantiates the roster functionally. */
template<typename Session_t, typename = void>
struct Has_app_shm : std::false_type {};
template<typename Session_t>
struct Has_app_shm<Session_t, std::void_t<decltype(std::declval<Session_t&>().app_shm())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_session_shm : std::false_type {};
template<typename Session_t>
struct Has_session_shm<Session_t, std::void_t<decltype(std::declval<Session_t&>().session_shm())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_session_shm_ptr : std::false_type {};
template<typename Session_t>
struct Has_session_shm_ptr<Session_t, std::void_t<decltype(std::declval<Session_t&>().session_shm_ptr())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_app_shm_ptr : std::false_type {};
template<typename Session_t>
struct Has_app_shm_ptr<Session_t, std::void_t<decltype(std::declval<Session_t&>().app_shm_ptr())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_shm_session : std::false_type {};
template<typename Session_t>
struct Has_shm_session<Session_t, std::void_t<decltype(std::declval<Session_t&>().shm_session())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_shm_reader_config : std::false_type {};
template<typename Session_t>
struct Has_shm_reader_config<Session_t, std::void_t<decltype(std::declval<Session_t&>().shm_reader_config())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_app_shm_builder_config : std::false_type {};
template<typename Session_t>
struct Has_app_shm_builder_config
         <Session_t, std::void_t<decltype(std::declval<Session_t&>().app_shm_builder_config())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_app_shm_lender_session : std::false_type {};
template<typename Session_t>
struct Has_app_shm_lender_session
         <Session_t, std::void_t<decltype(std::declval<Session_t&>().app_shm_lender_session())>> :
  std::true_type {};
template<typename Session_t, typename = void>
struct Has_app_shm_reader_config : std::false_type {};
template<typename Session_t>
struct Has_app_shm_reader_config
         <Session_t, std::void_t<decltype(std::declval<Session_t&>().app_shm_reader_config())>> :
  std::true_type {};
template<typename Server_t, typename = void>
struct Has_server_app_shm : std::false_type {};
template<typename Server_t>
struct Has_server_app_shm
         <Server_t, std::void_t<decltype(std::declval<Server_t&>().app_shm(std::declval<const Client_app&>()))>> :
  std::true_type {};
template<typename Server_t, typename = void>
struct Has_server_app_shm_ptr : std::false_type {};
template<typename Server_t>
struct Has_server_app_shm_ptr
         <Server_t,
          std::void_t<decltype(std::declval<Server_t&>().app_shm_ptr(std::declval<const Client_app&>()))>> :
  std::true_type {};
template<typename Server_t, typename = void>
struct Has_server_app_shm_builder_config : std::false_type {};
template<typename Server_t>
struct Has_server_app_shm_builder_config
         <Server_t,
          std::void_t<decltype(std::declval<Server_t&>()
                                 .app_shm_builder_config(std::declval<const Client_app&>()))>> :
  std::true_type {};
template<typename Server_t, typename = void>
struct Has_server_app_shm_lender_session : std::false_type {};
template<typename Server_t>
struct Has_server_app_shm_lender_session
         <Server_t,
          std::void_t<decltype(std::declval<Server_t&>()
                                 .app_shm_lender_session(std::declval<const Client_app&>()))>> :
  std::true_type {};
template<typename Server_t, typename = void>
struct Has_server_app_shm_reader_config : std::false_type {};
template<typename Server_t>
struct Has_server_app_shm_reader_config
         <Server_t,
          std::void_t<decltype(std::declval<Server_t&>()
                                 .app_shm_reader_config(std::declval<const Client_app&>()))>> :
  std::true_type {};

// Readable per-instantiation test names (in place of gtest's default /0, /1, /2).
struct Pair_type_names
{
  template<typename Session_pair_t>
  static string GetName(int) // (Name style is an exception: mandated by gtest.)
  {
    switch (Session_pair_t::S_SHM_TYPE_OR_NONE)
    {
      case schema::ShmType::NONE: return "vanilla";
      case schema::ShmType::CLASSIC: return "shmClassic";
      case schema::ShmType::JEMALLOC: return "shmJemalloc";
      default: assert(false);
    }
    return {};
  }
};

// Google test fixture; parameterized on the Session_pair specialization = the session type under test.
template<typename Session_pair_t>
class Session_connect_test :
  public ::testing::Test,
  public flow::log::Log_context
{
public:
  using Pair = Session_pair_t;
  using Client_session = typename Pair::Client_session_t;
  using Session_server = typename Pair::Session_server_t;
  using Server_session = typename Pair::Server_session_t;
  using Channel_obj = typename Client_session::Channel_obj;

  Session_connect_test() :
    flow::log::Log_context(&m_test_logger, Log_component::S_TEST)
  {
    /* Global-logger contract for the SHM-jemalloc machinery (see ipc::session::shm::arena_lend public
     * docs: it is essentially a global, so non-global session objects do not impose their individual
     * loggers on it).  Harmless for the other two session types. */
    ipc::shm::arena_lend::set_logger(ipc_logger());
  }

  ~Session_connect_test() override
  {
    ipc::shm::arena_lend::set_logger(nullptr);
  }

  /* The logger passed to the Flow-IPC objects under test.  Null = quiet, the default: session open/close
   * alone produces pages of detailed logging.  Flip the #if to eyeball all of it when debugging. */
  flow::log::Logger* ipc_logger()
  {
#if 1
    return nullptr;
#else
    return &m_test_logger;
#endif
  }

  /* Constructs (into `*pair.m_srv`) a Session_server for the pair's App universe; it listens immediately.
   *
   * SHM-classic: unless `cap_shm_pools` is false, each server's pool size limit is set to a small value (megabytes).
   * (If the current impl by default commits the entire size of SHM-pool (as opposed to merely sparse-mapping it), the
   * default size (gigabytes as of this writing) means (1) tests slow down ~100x, and (2) massive temporary RAM
   * use, and (3) because of that possible async_accept() failure (hard no-space-left error in Linux at least).
   * If the current impl commits only as-needed, then this is unnecessary but harmless.) */
  void start_server(Pair* pair, [[maybe_unused]] bool cap_shm_pools = true)
  {
    pair->m_srv = std::make_unique<Session_server>(ipc_logger(),
                                                   pair->m_srv_apps.find(pair->m_srv_app.m_name)->second,
                                                   pair->m_cli_apps);
    if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::CLASSIC)
    {
      if (cap_shm_pools)
      {
        pair->m_srv->pool_size_limit_mi(S_SHM_CLASSIC_POOL_SIZE_LIMIT_MI);
      }
    }
  }

  // See start_server().  Ample for this file's uses of SHM (a few small objects per test).
  static constexpr size_t S_SHM_CLASSIC_POOL_SIZE_LIMIT_MI = 16;

  /* Holders for an async_accept() outcome (see post_accept()).  shared_ptr semantics on purpose: if a
   * test bails (ASSERT) with the accept still outstanding, the handler shall still fire eventually (at
   * the latest with operation-aborted at server destruction) and must not write into then-dead locals. */
  struct Accept_outcome
  {
    boost::shared_ptr<Error_code> m_err{boost::make_shared<Error_code>()};
    boost::shared_ptr<boost::promise<void>> m_done{boost::make_shared<boost::promise<void>>()};
  };

  // Kicks off async_accept() on `*pair.m_srv` (the no-init-channels/no-MDT shape) targeting *srv_session.
  Accept_outcome post_accept(Pair* pair, Server_session* srv_session)
  {
    Accept_outcome outcome;
    pair->m_srv->async_accept(srv_session,
                              nullptr, // init_channels_by_srv_req (none)
                              nullptr, // mdt_from_cli_or_null
                              nullptr, // init_channels_by_cli_req (client requests none)
                              [](auto&&...) { return 0; }, // n_init_channels_by_srv_req_func
                              [](auto&&...) {},            // mdt_load_func
                              [outcome](const Error_code& err_code)
    {
      *outcome.m_err = err_code;
      outcome.m_done->set_value();
    });
    return outcome;
  }

  // Awaits the post_accept() outcome, loading it into *err_code; test failure if it takes too long.
  void await_accept(const Accept_outcome& outcome, Error_code* err_code)
  {
    using boost::chrono::seconds;

    ASSERT_EQ(outcome.m_done->get_future().wait_for(seconds(5)), boost::future_status::ready)
      << "async_accept handler did not fire in time.";
    *err_code = *outcome.m_err;
  }

  /* Performs a full successful session-establishment: async_accept() posted on `*pair.m_srv` (which must
   * exist/listen) targeting `*srv_session`; then `*cli` (which must be a constructed, NULL-state
   * Client_session) sync_connect()s; both sides are verified to reach PEER (server side via
   * init_handlers() with a no-op error handler).  Single-threaded on purpose: async_accept() is
   * non-blocking, and its machinery (as well as all of the log-in legwork on both sides, including the
   * SHM-provider hooks where applicable) runs on the session objects' internal threads while
   * sync_connect() blocks here. */
  void connect_ok(Pair* pair, Client_session* cli, Server_session* srv_session)
  {
    const auto outcome = post_accept(pair, srv_session);

    Error_code err_code;
    const bool ok = cli->sync_connect(cli->mdt_builder(), nullptr, nullptr, nullptr, &err_code);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(err_code) << "sync_connect error: [" << err_code << "] [" << err_code.message() << "].";

    Error_code accept_err;
    await_accept(outcome, &accept_err);
    EXPECT_FALSE(accept_err) << "async_accept error: [" << accept_err << "] ["
                             << accept_err.message() << "].";

    srv_session->init_handlers([](const Error_code&) {});
  } // connect_ok()

  /* Drives `*cli` (constructed, NULL-state; possibly of a foreign Client_session type -- hence the
   * template) into a connect that the server must reject: verifies the client observes *some* error
   * (by design the server closes the session master channel without a response, volunteering nothing
   * to a peer it just rejected, so nothing more specific is available client-side) and that the
   * server's accept handler receives exactly `expected_code`. */
  template<typename Any_client_session>
  void connect_expecting_server_reject(Pair* pair, Server_session* srv_session,
                                       Any_client_session* cli, error::Code expected_code)
  {
    const auto outcome = post_accept(pair, srv_session);

    Error_code err_code;
    const bool ok = cli->sync_connect(cli->mdt_builder(), nullptr, nullptr, nullptr, &err_code);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(err_code);

    Error_code accept_err;
    await_accept(outcome, &accept_err);
    EXPECT_TRUE(accept_err == expected_code)
      << "Expected server-side reject code [" << Error_code{expected_code} << "]; got: ["
      << accept_err << "] [" << accept_err.message() << "].";
  } // connect_expecting_server_reject()

  /* The graceful-session-end choreography + observations, in either direction (the two directions are
   * symmetric per the Session concept; the impl is asymmetric enough to warrant running both).  In one
   * go this covers, with `initiator` = the side whose Session dtor runs first, `observer` = the other:
   *   - The observer's on-error handler fires exactly once: with SESSION_FINISHED for SHM-jemalloc
   *     (carried by the internal GracefulSessionEnd message) -- or, for the other session types, the
   *     generic master-channel graceful-close error (logged; deliberately not asserted-exactly).
   *   - The initiator's own handler never fires: locally-triggered destruction emits no error.
   *     Nor does the observer's fire a 2nd time due to its own eventual dtor.
   *   - SHM-jemalloc only: the initiator's dtor demonstrably *blocks* until the observer's dtor begins
   *     (the Session_base::Graceful_finisher gate; see its doc header for all the background ever);
   *     for the other session types the dtor completes promptly with the observer still alive.
   *   - The hosed observer's APIs behave per contract: open_channel() false/no-op, session_token()
   *     nil, mdt_builder() null.
   * Kernel-persistent leftovers are deliberately not scanned-for here: that (including for the graceful
   * whole-lifecycle case) is covered by session_persistent_cleanup_test.cpp. */
  void run_graceful_end(bool srv_initiates)
  {
    namespace this_thread = flow::util::this_thread;
    using flow::async::Single_thread_task_loop;
    using boost::chrono::milliseconds;
    using std::atomic;

    const auto pair_ptr = boost::make_shared<Pair>();
    auto& pair = *pair_ptr;
    pair.populate_apps(srv_initiates ? "GraceEndSrv" : "GraceEndCli");
    pair.remove_server_persistent_bits(false);

    /* Error-handler outcome holders, one set per side.  shared_ptr captures for the usual reason: no
     * handler may ever fire into dead locals, even on an ASSERT bail-out. */
    const auto cli_hose_count = boost::make_shared<atomic<int>>(0);
    const auto cli_hose_code = boost::make_shared<Error_code>();
    const auto cli_hosed = boost::make_shared<boost::promise<void>>();
    const auto on_cli_err = [cli_hose_count, cli_hose_code, cli_hosed](const Error_code& err_code)
    {
      if (cli_hose_count->fetch_add(1) == 0)
      {
        *cli_hose_code = err_code;
        cli_hosed->set_value();
      }
    };
    const auto srv_hose_count = boost::make_shared<atomic<int>>(0);
    const auto srv_hose_code = boost::make_shared<Error_code>();
    const auto srv_hosed = boost::make_shared<boost::promise<void>>();
    const auto on_srv_err = [srv_hose_count, srv_hose_code, srv_hosed](const Error_code& err_code)
    {
      if (srv_hose_count->fetch_add(1) == 0)
      {
        *srv_hose_code = err_code;
        srv_hosed->set_value();
      }
    };

    /* Bail-out-safety choreography notes.  The initiator's dtor runs on ender_thread; under SHM-jemalloc
     * it blocks until the observer's dtor begins.  Should an ASSERT bail us out mid-test:
     *   - The sessions are heap-pinned via shared_ptr, and the posted task captures its target by
     *     shared_ptr: so the mid-dtor object cannot be yanked out from under the task by stack unwind.
     *   - ~Single_thread_task_loop joins the task; by then the unwind has released the observer's
     *     shared_ptr, whose dtor satisfied the gate (the initiator had, at the least, already announced
     *     its own dtor-start): no deadlock. */
    start_server(&pair);
    Single_thread_task_loop ender_thread{nullptr, "gf_ender"};
    ender_thread.start();
    const auto srv_session_ptr = boost::make_shared<Server_session>();
    const auto cli_ptr = boost::make_shared<Client_session>();

    const auto outcome = post_accept(&pair, srv_session_ptr.get());
    *cli_ptr = Client_session{ipc_logger(), pair.m_cli_app, pair.m_srv_app, on_cli_err};
    Error_code err_code;
    EXPECT_TRUE(cli_ptr->sync_connect(cli_ptr->mdt_builder(), nullptr, nullptr, nullptr, &err_code));
    EXPECT_FALSE(err_code);
    Error_code accept_err;
    await_accept(outcome, &accept_err);
    EXPECT_FALSE(accept_err);
    srv_session_ptr->init_handlers(on_srv_err);

    // Both sides in PEER state.  Now the actual subject matter.
    FLOW_LOG_INFO("Session up.  The [" << (srv_initiates ? "server" : "client") << "] side's Session dtor "
                  "begins (on a helper thread: for SHM-jemalloc it shall block; that is the point).");
    const auto ender_done = boost::make_shared<atomic<bool>>(false);
    if (srv_initiates)
    {
      ender_thread.post([srv_session_ptr, ender_done]()
      {
        *srv_session_ptr = Server_session{};
        ender_done->store(true);
      });
    }
    else
    {
      ender_thread.post([cli_ptr, ender_done]()
      {
        *cli_ptr = Client_session{};
        ender_done->store(true);
      });
    }

    // The observer must learn of the session's end via its error handler.
    {
      const auto& observer_hosed = srv_initiates ? cli_hosed : srv_hosed;
      ASSERT_EQ(observer_hosed->get_future().wait_for(boost::chrono::seconds(5)),
                boost::future_status::ready)
        << "Observer side's error handler did not fire though opposing Session dtor started.";
    }
    const auto observer_hose_code = srv_initiates ? *cli_hose_code : *srv_hose_code;
    if constexpr (Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::JEMALLOC)
    {
      EXPECT_TRUE(observer_hose_code == error::Code::S_SESSION_FINISHED)
        << "Expected the specific graceful-session-end code; got: [" << observer_hose_code << "] ["
        << observer_hose_code.message() << "].";
    }
    else
    {
      EXPECT_TRUE(observer_hose_code);
      FLOW_LOG_INFO("Observer-side handler fired with [" << observer_hose_code << "] ["
                    << observer_hose_code.message() << "] (the generic master-channel graceful-close "
                    "error; its exact value is deliberately not asserted).");
    }

    if constexpr (Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::JEMALLOC)
    {
      /* The Graceful_finisher gate: the initiator's dtor must still be blocked -- the observer's dtor
       * has not begun.  (A sleep proves a negative only so-so; but combined with the affirmative
       * gate-release check below, the coverage is real.) */
      this_thread::sleep_for(milliseconds(250));
      EXPECT_FALSE(ender_done->load()) << "SHM-jemalloc Session dtor must await the opposing dtor.";
    }
    else
    {
      // No gate outside SHM-jemalloc: the initiator's dtor completes with the observer still alive.
      for (int i = 0; (i != 50) && (!ender_done->load()); ++i)
      {
        this_thread::sleep_for(milliseconds(100));
      }
      EXPECT_TRUE(ender_done->load()) << "Non-SHM-jemalloc Session dtor must not block on opposing dtor.";
    }

    /* The hosed observer's APIs, per contract: no-ops/sentinels.  (Only the contract-backed subset:
     * session_token() and open_channel() docs specify hosed-session behavior; mdt_builder()'s
     * null-return is specified for not-in-PEER only, and a hosed session is formally still in
     * irreversible PEER state -- so that one is not probed.) */
    const auto probe_hosed_api = [&](auto& observer)
    {
      EXPECT_TRUE(observer.session_token().is_nil());
      Channel_obj chan;
      Error_code chan_err;
      EXPECT_FALSE(observer.open_channel(&chan, &chan_err));
    };
    srv_initiates ? probe_hosed_api(*cli_ptr) : probe_hosed_api(*srv_session_ptr);

    FLOW_LOG_INFO("Observer session's dtor now; the initiator's (if gated) must thereby complete.");
    if (srv_initiates)
    {
      *cli_ptr = Client_session{};
    }
    else
    {
      *srv_session_ptr = Server_session{};
    }
    for (int i = 0; (i != 50) && (!ender_done->load()); ++i)
    {
      this_thread::sleep_for(milliseconds(100));
    }
    EXPECT_TRUE(ender_done->load()) << "Initiator's Session dtor never completed.";
    ender_thread.stop();

    // The full handler tally: observer exactly once (its own dtor did not re-fire it); initiator never.
    EXPECT_EQ((srv_initiates ? cli_hose_count : srv_hose_count)->load(), 1);
    EXPECT_EQ((srv_initiates ? srv_hose_count : cli_hose_count)->load(), 0);

    pair.m_srv.reset();
    pair.remove_server_persistent_bits();
  } // run_graceful_end()

  /* The opposing (passive) side's user closes each just-passive-opened channel end immediately, while the active
   * side's open_channel() may still be attaching to the channel's resources.  `srv_opens` selects the direction:
   * the server actively opens (and the client's user closes) or vice versa.
   *
   * The race: destroying either MQ pipe-end unlinks the MQ's name (see Blob_stream_mq_sender docs), and the passive
   * side hands the end to its user right after sending the response.  When the client is the active side, its
   * attach races that unlink and, on losing, cannot open the MQ.  Per contract that is a non-fatal open_channel()
   * outcome: the call returns `true` with a system error (ENOENT on the MQ open), the session survives, and the
   * target channel is untouched.  Winning the race is fine too: the active side gets a channel whose peer end is
   * already gone, which is the user's business.  When the server is the active side there is no such race today:
   * it creates the resources itself, before its request goes out.  We deliberately do not rely on that
   * impl-level fact: both directions are held to the same outcomes.
   *
   * Asserted: each open yields exactly one of those two outcomes -- never anything else, in particular never an
   * abort; every request reaches the passive side's handler; the session is intact afterwards.  How often the race
   * is lost is logged for information, not asserted.
   *
   * Skipped under TSAN for the same reason as Open_channel_crossing (rapid cross-thread descriptor churn). */
  void run_peer_closes_immediately(bool srv_opens)
  {
    using boost::system::errc::no_such_file_or_directory;
    using std::atomic;

    if constexpr(flow::test::tsan_enabled())
    {
      GTEST_SKIP() << "Skipped under ThreadSanitizer: descriptor-number-reuse false positives; see "
                      "Open_channel_crossing's doc comment.";
    }

    constexpr size_t N_OPENS = 20;
    const string active_side = srv_opens ? "server" : "client";

    const auto pair_ptr = boost::make_shared<Pair>();
    auto& pair = *pair_ptr;
    pair.populate_apps(srv_opens ? "PeerClosesS" : "PeerClosesC");
    pair.remove_server_persistent_bits(false);

    // The passive side's handler lets each new channel end die on the spot.  (Runs on that session's thread.)
    const auto n_passive = boost::make_shared<atomic<size_t>>(0);
    const auto on_passive_open = [n_passive](Channel_obj&& /*new_chan*/, auto&& /*mdt_reader*/) { ++*n_passive; };

    // Only the passive side gets a passive-open handler.
    start_server(&pair);
    Server_session srv_session;
    const auto outcome = post_accept(&pair, &srv_session);
    auto cli = srv_opens
                 ? Client_session{ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {},
                                  on_passive_open}
                 : Client_session{ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
    Error_code err_code;
    EXPECT_TRUE(cli.sync_connect(cli.mdt_builder(), nullptr, nullptr, nullptr, &err_code));
    EXPECT_FALSE(err_code) << "sync_connect error: [" << err_code << "] [" << err_code.message() << "].";
    Error_code accept_err;
    await_accept(outcome, &accept_err);
    ASSERT_FALSE(accept_err) << "async_accept error: [" << accept_err << "] [" << accept_err.message() << "].";
    if (srv_opens)
    {
      srv_session.init_handlers([](const Error_code&) {});
    }
    else
    {
      srv_session.init_handlers([](const Error_code&) {}, on_passive_open);
    }

    const auto open_one = [&](Channel_obj* chan, Error_code* chan_err) -> bool
    {
      return srv_opens ? srv_session.open_channel(chan, chan_err) : cli.open_channel(chan, chan_err);
    };

    size_t n_attached = 0;
    size_t n_yanked = 0;
    for (size_t idx = 0; idx != N_OPENS; ++idx)
    {
      Channel_obj chan;
      Error_code chan_err;
      ASSERT_TRUE(open_one(&chan, &chan_err))
        << "Open #" << idx << " by [" << active_side << "]: open_channel() reported not carried out.";
      if (!chan_err)
      {
        ++n_attached;
      }
      else
      {
        EXPECT_TRUE(chan_err == no_such_file_or_directory)
          << "Open #" << idx << " by [" << active_side << "]: expected the peer-closed-early system error; got: "
             "[" << chan_err << "] [" << chan_err.message() << "].";
        ++n_yanked;
      }
    }
    FLOW_LOG_INFO("Of [" << N_OPENS << "] opens by the [" << active_side << "] against a peer that closes "
                  "immediately: [" << n_attached << "] attached (race won), [" << n_yanked << "] refused with the "
                  "peer-closed-early error (race lost).  Either is fine.");

    // No worse for wear?
    EXPECT_FALSE(cli.session_token().is_nil());
    EXPECT_EQ(srv_session.session_token(), cli.session_token());
    Channel_obj chan;
    Error_code chan_err;
    EXPECT_TRUE(open_one(&chan, &chan_err)); // One more, for good measure: still no abort/hosing.
    EXPECT_TRUE((!chan_err) || (chan_err == no_such_file_or_directory)) << "[" << chan_err << "].";
    chan = Channel_obj{};
    /* Every request reached the passive side and was passive-opened there.  (It invokes its handler after sending
     * the response, so the last invocation may trail our last open_channel() return by a moment.) */
    for (size_t idx = 0; (idx != 500) && (n_passive->load() != (N_OPENS + 1)); ++idx)
    {
      flow::util::this_thread::sleep_for(boost::chrono::milliseconds(10));
    }
    EXPECT_EQ(n_passive->load(), N_OPENS + 1);

    pair.destroy_sessions(&cli, &srv_session);
    pair.m_srv.reset();
    pair.remove_server_persistent_bits();
  } // run_peer_closes_immediately()

protected:
  // For the test's own narration (INFO); also the optional ipc_logger() target.
  Test_logger m_test_logger{Sev::S_INFO};
}; // class Session_connect_test

TYPED_TEST_SUITE_P(Session_connect_test);

/* No server has ever run: no CNS (PID file) exists.  Connect must fail with the OS file-not-found error;
 * and the failure must leave the Client_session object reusable: once a server is up, the same object
 * connects successfully. */
TYPED_TEST_P(Session_connect_test, No_server)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;

  // Heap-pin the pair: the sessions store the App members by address (see test_util.hpp).
  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("NoSrv");
  pair.remove_server_persistent_bits(false); // Pre-clean a previous run's possible leavings: pristine start.

  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};

  FLOW_LOG_INFO("Connect attempt with no server ever having run (hence no CNS (PID file)).");
  Error_code err_code;
  const bool ok = cli.sync_connect(cli.mdt_builder(), nullptr, nullptr, nullptr, &err_code);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(err_code == boost::system::errc::no_such_file_or_directory)
    << "Expected the OS file-not-found error; got: [" << err_code << "] [" << err_code.message() << "].";

  FLOW_LOG_INFO("Failed as expected.  Bringing up a server; the same Client_session object retries.");
  this->start_server(&pair);
  Server_session srv_session;
  this->connect_ok(&pair, &cli, &srv_session);

  pair.destroy_sessions(&cli, &srv_session); // (Overload for caller-owned sessions; GF-aware for SHMJ.)
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* The CNS (PID file) exists but is corrupt.  Each of the two malformed-contents code paths must yield
 * CLIENT_NAMESPACE_STORE_BAD_FORMAT: contents that are a line but not a number; contents lacking a
 * newline-terminated line at all.  (No server is involved: the client bails before any socket work.) */
TYPED_TEST_P(Session_connect_test, Corrupt_cns)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("BadCns");
  pair.remove_server_persistent_bits(false);

  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};

  const auto cns_path = pair.cns_path();
  const auto connect_expecting_bad_format = [&](const string& cns_contents)
  {
    {
      std::ofstream cns_file{cns_path.string()};
      cns_file << cns_contents;
      EXPECT_TRUE(cns_file.good());
    }
    Error_code err_code;
    const bool ok = cli.sync_connect(cli.mdt_builder(), nullptr, nullptr, nullptr, &err_code);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(err_code == error::Code::S_CLIENT_NAMESPACE_STORE_BAD_FORMAT)
      << "CNS contents [" << cns_contents << "] should have yielded BAD_FORMAT; got: [" << err_code << "] "
         "[" << err_code.message() << "].";
  };

  FLOW_LOG_INFO("Connect attempts against corrupt CNS (PID file) [" << cns_path << "]: 2 corruption sorts.");
  connect_expecting_bad_format("notanumber\n"); // Proper line; does not parse as a PID.
  connect_expecting_bad_format("12345"); // Would parse fine; but no newline-terminated line = not proper.

  /* Clean slate: the CNS file (hand-made above) and its mutex (created by the client during the attempts)
   * both exist; standard removal applies. */
  pair.remove_server_persistent_bits();
}

/* The stale-CNS scenario -- in production terms: server instance 1 ran and is gone (cleanly or not: for
 * a client the observable is the same); the CNS (PID file) remains -- nothing ever deletes it, on purpose;
 * each successive server instance overwrites it in place.  A client connect during the between-servers
 * window reads the CNS fine but cannot reach the dead instance's socket-acceptor.  Once server instance 2
 * is up, a retry -- by the *same* Client_session object -- must succeed.
 *
 * The regression under test (white-box): the stale attempt fails *after* the CNS read has irreversibly(?)
 * recorded the server-namespace in the object; the internal rollback-to-NULL-state must clear that record,
 * or the retry's own CNS read cannot re-record it (historically: an assert-trip in a Debug build).  So the
 * essential sequence is fail-after-CNS-read, then retry: which is exactly what this test arranges. */
TYPED_TEST_P(Session_connect_test, Stale_cns_then_retry)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("StaleCns");
  pair.remove_server_persistent_bits(false);

  // Baseline: server instance 1; a client connects fine (establishing: the CNS is present and well-formed).
  FLOW_LOG_INFO("Server instance 1 up; baseline connect.");
  this->start_server(&pair);
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Server_session srv_session;
  this->connect_ok(&pair, &cli, &srv_session);
  pair.destroy_sessions(&cli, &srv_session);

  // Server instance 1 dies.  Its socket-acceptor dies with it; the CNS file + mutex remain (by design).
  pair.m_srv.reset();

  FLOW_LOG_INFO("Server instance 1 gone; connect attempt against the now-stale CNS.");
  cli = Client_session{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Error_code err_code;
  bool ok = cli.sync_connect(cli.mdt_builder(), nullptr, nullptr, nullptr, &err_code);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(err_code);
  FLOW_LOG_INFO("Stale-CNS attempt failed -- good -- with [" << err_code << "] [" << err_code.message()
                << "] (the exact OS-level code is deliberately not asserted; the failure mode is "
                   "cannot-reach-acceptor, downstream of a successful CNS read).");

  FLOW_LOG_INFO("Server instance 2 up (CNS overwritten in place); same Client_session object retries.");
  this->start_server(&pair);
  this->connect_ok(&pair, &cli, &srv_session);

  // Beyond PEER-ness: a small functionality check of the freshly established session.
  EXPECT_FALSE(cli.session_token().is_nil());
  EXPECT_EQ(cli.session_token(), srv_session.session_token());

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* Identity/authorization rejects at log-in.  Wanted behavior, per sub-case: the server emits the
 * specific reject code to its async_accept() handler; the client observes its connect fail (with
 * nothing more specific than channel-death-during-log-in: the server tells a rejectee nothing, by
 * design); and -- the key resilience aspect -- the server takes no damage: at the end a proper
 * client connects fine.
 *
 * Sub-cases and their expected server-side codes:
 *   - Client_app name entirely unknown to the server: DISALLOWED_OR_UNKNOWN.
 *   - Client_app registered, but absent from the Server_app's allowed-client-apps list: ditto.
 *   - Client_app registered and allowed, but registered with a UID other than the connecting
 *     process's: INCONSISTENT_CREDS.
 *   - Ditto, but with a wrong registered executable path: INCONSISTENT_CREDS. */
TYPED_TEST_P(Session_connect_test, Rejected_identities)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("Rej");
  pair.remove_server_persistent_bits(false);

  /* Extend the App universe -- before the server starts (it stores the registries by address) -- with
   * the impostor cast.  In each case the *client-side* Client_app copy below is truthful (otherwise
   * client-side machinery might be what fails, which is not the subject); the corruption, where
   * applicable, is in the server-side registered copy: the server does the rejecting. */
  Client_app cli_app_unknown{pair.m_cli_app}; // Truthful; simply never registered server-side.
  cli_app_unknown.m_name += "Unk";
  Client_app cli_app_disallowed{pair.m_cli_app}; // Truthful; registered; but not in the allowed list.
  cli_app_disallowed.m_name += "Dis";
  Client_app cli_app_bad_uid{pair.m_cli_app}; // Truthful; its *registered* copy shall claim another UID.
  cli_app_bad_uid.m_name += "BadUid";
  Client_app cli_app_bad_path{pair.m_cli_app}; // Truthful; its *registered* copy: another exec path.
  cli_app_bad_path.m_name += "BadPath";
  {
    auto registered_bad_uid = cli_app_bad_uid;
    registered_bad_uid.m_user_id += 1;
    auto registered_bad_path = cli_app_bad_path;
    registered_bad_path.m_exec_path = "/bin/some/other/binary";
    pair.m_cli_apps.insert({{cli_app_disallowed.m_name, cli_app_disallowed},
                            {registered_bad_uid.m_name, registered_bad_uid},
                            {registered_bad_path.m_name, registered_bad_path}});
    pair.m_srv_app.m_allowed_client_apps.insert(cli_app_bad_uid.m_name);
    pair.m_srv_app.m_allowed_client_apps.insert(cli_app_bad_path.m_name);
    // (cli_app_disallowed.m_name deliberately not added to the allowed list; cli_app_unknown: nowhere.)

    /* Attention: the server shall be constructed off the m_srv_apps master-set *copy* of m_srv_app
     * (see start_server()), snapshotted by populate_apps() before the above mutation: refresh it. */
    pair.m_srv_apps = Server_app::Master_set{{pair.m_srv_app.m_name, pair.m_srv_app}};
  }

  FLOW_LOG_INFO("Server up; the 4 reject sub-cases follow.  (Server-side WARNING details are available "
                "by flipping the Flow-IPC logger on.)");
  this->start_server(&pair);
  Server_session srv_session;

  {
    Client_session cli{this->ipc_logger(), cli_app_unknown, pair.m_srv_app, [](const Error_code&) {}};
    this->connect_expecting_server_reject
      (&pair, &srv_session, &cli, error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN);
  }
  {
    Client_session cli{this->ipc_logger(), cli_app_disallowed, pair.m_srv_app, [](const Error_code&) {}};
    this->connect_expecting_server_reject
      (&pair, &srv_session, &cli, error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_DISALLOWED_OR_UNKNOWN);
  }
  {
    Client_session cli{this->ipc_logger(), cli_app_bad_uid, pair.m_srv_app, [](const Error_code&) {}};
    this->connect_expecting_server_reject
      (&pair, &srv_session, &cli, error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS);
  }
  {
    Client_session cli{this->ipc_logger(), cli_app_bad_path, pair.m_srv_app, [](const Error_code&) {}};
    this->connect_expecting_server_reject
      (&pair, &srv_session, &cli, error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CLIENT_APP_INCONSISTENT_CREDS);
  }

  FLOW_LOG_INFO("All 4 rejected as expected.  The server must have taken no damage: proper client connects.");
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  this->connect_ok(&pair, &cli, &srv_session);

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* The server's own identity self-check at construction: the Server_app handed to the Session_server ctor
 * must describe the actual process, or the ctor fails -- before creating anything kernel-persistent -- with
 * the specific code.  (Every opposing client checks the same things about the server and would refuse every
 * session; the self-check makes the misconfiguration fail early and clearly instead.)  Sub-cases, each
 * Server_app differing from the truthful one in one aspect:
 *   - Wrong UID: RESOURCE_OWNER_UNEXPECTED.
 *   - Wrong executable path: SERVER_APP_EXEC_PATH_INCONSISTENT.
 *   - The same binary, but its path spelled differently (an extra `.` component): ditto -- the documented
 *     match is exact, not by file identity.
 *   - Wrong executable path again, but via the throwing ctor form: Runtime_error carrying that code.
 * In each case the failed object's destruction must be harmless.  Lastly a server built off the truthful
 * Server_app works: a client connects. */
TYPED_TEST_P(Session_connect_test, Server_self_check)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Session_server = typename TestFixture::Session_server;
  using Server_session = typename TestFixture::Server_session;
  using flow::error::Runtime_error;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("SrvSelf");
  pair.remove_server_persistent_bits(false);

  auto srv_app_bad_uid = pair.m_srv_app;
  srv_app_bad_uid.m_user_id += 1;
  auto srv_app_bad_path = pair.m_srv_app;
  srv_app_bad_path.m_exec_path = "/bin/some/other/binary";
  auto srv_app_respelled_path = pair.m_srv_app;
  srv_app_respelled_path.m_exec_path = pair.m_srv_app.m_exec_path.parent_path() / "."
                                         / pair.m_srv_app.m_exec_path.filename();
  ASSERT_NE(srv_app_respelled_path.m_exec_path.string(), pair.m_srv_app.m_exec_path.string());

  const auto expect_ctor_error = [&](const Server_app& srv_app, error::Code expected_code)
  {
    Error_code err_code;
    {
      Session_server srv{this->ipc_logger(), srv_app, pair.m_cli_apps, &err_code};
    } // The failed object's dtor runs here.
    EXPECT_TRUE(err_code == expected_code)
      << "Server_app exec path [" << srv_app.m_exec_path << "], UID [" << srv_app.m_user_id << "]: expected "
         "ctor error [" << Error_code{expected_code} << "]; got: [" << err_code << "] [" << err_code.message() << "].";
  };

  FLOW_LOG_INFO("Constructing servers off miscast Server_apps; each must fail with the specific code.");
  expect_ctor_error(srv_app_bad_uid, error::Code::S_RESOURCE_OWNER_UNEXPECTED);
  expect_ctor_error(srv_app_bad_path, error::Code::S_SERVER_APP_EXEC_PATH_INCONSISTENT);
  expect_ctor_error(srv_app_respelled_path, error::Code::S_SERVER_APP_EXEC_PATH_INCONSISTENT);

  bool threw = false;
  try
  {
    Session_server srv{this->ipc_logger(), srv_app_bad_path, pair.m_cli_apps};
  }
  catch (const Runtime_error& exc)
  {
    threw = true;
    EXPECT_TRUE(exc.code() == error::Code::S_SERVER_APP_EXEC_PATH_INCONSISTENT)
      << "Got: [" << exc.code() << "] [" << exc.code().message() << "].";
  }
  EXPECT_TRUE(threw);

  FLOW_LOG_INFO("All rejected as expected.  A server off the truthful Server_app must work.");
  this->start_server(&pair);
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Server_session srv_session;
  this->connect_ok(&pair, &cli, &srv_session);

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* A client built with different compile-time session-config template parameters than the server's:
 * the log-in handshake transmits the client's config, and the server must reject on the mismatch --
 * CONFIG_MISMATCH server-side, generic connect failure client-side, and no damage to the server.
 * We cross the MQ-type knob (bipc-MQ client versus our standard POSIX-MQ server); the server-side
 * check is a single combined comparison of all the config knobs, so one crossing exercises it. */
TYPED_TEST_P(Session_connect_test, Config_mismatch)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Mismatched_client
    = typename Session_pair<schema::MqType::BIPC, true, TypeParam::S_SHM_TYPE_OR_NONE>::Client_session_t;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("CfgMm");
  pair.remove_server_persistent_bits(false);

  FLOW_LOG_INFO("Server up; a bipc-MQ-configured client connects to our POSIX-MQ-configured server.");
  this->start_server(&pair);
  Server_session srv_session;
  {
    Mismatched_client cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
    this->connect_expecting_server_reject
      (&pair, &srv_session, &cli, error::Code::S_SERVER_MASTER_LOG_IN_REQUEST_CONFIG_MISMATCH);
  }

  FLOW_LOG_INFO("Rejected as expected.  Server must be unharmed: a properly-configured client connects.");
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  this->connect_ok(&pair, &cli, &srv_session);

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* A freshly-accepted Server_session is in almost-PEER state: init_handlers() has not yet been called.
 * Per contract, until then the Session-concept APIs no-op / return sentinels: open_channel() returns
 * false (a flat no-op), mdt_builder() returns null, session_token() returns the nil sentinel.  After
 * init_handlers(): PEER -- everything live (token non-nil and equal to the opposing side's). */
TYPED_TEST_P(Session_connect_test, Almost_peer)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("AlmostPeer");
  pair.remove_server_persistent_bits(false);

  this->start_server(&pair);
  Server_session srv_session;
  const auto outcome = this->post_accept(&pair, &srv_session);
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Error_code err_code;
  const bool ok = cli.sync_connect(cli.mdt_builder(), nullptr, nullptr, nullptr, &err_code);
  EXPECT_TRUE(ok);
  EXPECT_FALSE(err_code);
  Error_code accept_err;
  this->await_accept(outcome, &accept_err);
  EXPECT_FALSE(accept_err);

  FLOW_LOG_INFO("Server session emitted, in almost-PEER state; probing the documented API no-ops.");
  EXPECT_TRUE(srv_session.session_token().is_nil());
  EXPECT_FALSE(srv_session.mdt_builder());
  Channel_obj chan;
  Error_code chan_err;
  EXPECT_FALSE(srv_session.open_channel(&chan, &chan_err));
  // (false = flat no-op per contract; chan_err is not meaningful in that case.)

  FLOW_LOG_INFO("Entering PEER state via init_handlers(); probing everything came alive.");
  srv_session.init_handlers([](const Error_code&) {});
  EXPECT_FALSE(srv_session.session_token().is_nil());
  EXPECT_EQ(srv_session.session_token(), cli.session_token());
  EXPECT_TRUE(srv_session.mdt_builder());

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* NULL state: a default-cted session object -- and, equivalently per contract, a moved-from one -- has no
 * impl inside.  The Session-concept APIs return their sentinels: nil token, null metadata builder,
 * open_channel() false, the null-credentials sentinel (by reference identity, as documented), null
 * info_collector().  Beyond the concept: get_logger() returns null, and get_log_component() is usable (the
 * session component) -- both without an impl to consult.  Both sides; no server needed.
 *
 * Plus a compile-time check: the Structured_msg_reader_config alias is not (by copy-paste accident) the
 * builder-config type. */
TYPED_TEST_P(Session_connect_test, Null_state)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;

  static_assert(!std::is_same_v<typename Client_session::Structured_msg_reader_config,
                                typename Client_session::Structured_msg_builder_config>,
                "Reader-config alias must not be the builder-config type.");
  static_assert(!std::is_same_v<typename Server_session::Structured_msg_reader_config,
                                typename Server_session::Structured_msg_builder_config>,
                "Reader-config alias must not be the builder-config type.");

  const auto probe_null = [](auto& session, const string& ctx)
  {
    EXPECT_TRUE(session.session_token().is_nil()) << ctx;
    EXPECT_FALSE(session.mdt_builder()) << ctx;
    Channel_obj chan;
    EXPECT_FALSE(session.open_channel(&chan)) << ctx;
    EXPECT_EQ(&session.remote_peer_process_credentials(), &util::NULL_PROCESS_CREDENTIALS) << ctx;
    EXPECT_EQ(session.info_collector(), nullptr) << ctx;
    EXPECT_EQ(session.get_logger(), nullptr) << ctx;
    EXPECT_TRUE(session.get_log_component().template payload<Log_component>() == Log_component::S_SESSION) << ctx;
  };

  FLOW_LOG_INFO("Probing default-cted sessions (both sides).");
  Client_session cli_default;
  Server_session srv_default;
  probe_null(cli_default, "default-cted client");
  probe_null(srv_default, "default-cted server");

  FLOW_LOG_INFO("Probing a moved-from (never-connected) client session.");
  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("NullState");
  Client_session cli_src{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Client_session cli_dst{std::move(cli_src)};
  probe_null(cli_src, "moved-from client"); // As-if default-cted, per contract.
}

/* Channel passive-open rejection.  All clients in this file use the Client_session ctor form without
 * an on-passive-open handler: passive-opens disallowed on that side.  So a PEER-state server actively
 * opening a channel must have the attempt carried out (return true) yet yield the specific non-fatal
 * error in the out-arg.  Non-fatal is the point: the session must remain healthy -- shown by a second,
 * identically-rejected attempt (and intact tokens) rather than any hosing. */
TYPED_TEST_P(Session_connect_test, Passive_open_rejected)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("PassRej");
  pair.remove_server_persistent_bits(false);

  this->start_server(&pair);
  Server_session srv_session;
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  this->connect_ok(&pair, &cli, &srv_session);

  FLOW_LOG_INFO("Server actively opens a channel; the client (no passive-open handler) must reject.");
  const auto open_and_expect_reject = [&]()
  {
    Channel_obj chan;
    Error_code chan_err;
    EXPECT_TRUE(srv_session.open_channel(&chan, &chan_err));
    EXPECT_TRUE(chan_err == error::Code::S_SESSION_OPEN_CHANNEL_REMOTE_PEER_REJECTED_PASSIVE_OPEN)
      << "Expected the peer-rejected-passive-open error; got: [" << chan_err << "] ["
      << chan_err.message() << "].";
  };
  open_and_expect_reject();
  // Non-fatal indeed?  The session should be no worse for wear.
  open_and_expect_reject();
  EXPECT_FALSE(srv_session.session_token().is_nil());
  EXPECT_EQ(srv_session.session_token(), cli.session_token());

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* Crossing active-opens: both sides open_channel() repeatedly and concurrently (one thread per side; so each
 * session object still sees one non-const caller at a time, as the Session concept's thread-safety rules
 * require), each open passive-accepted by the opposing side.  This is a regression test.  open_channel()
 * is presented as synchronous and quick, but internally it awaits the opposing side's response; if that
 * response could only be produced by a thread that is itself blocked awaiting *our* response to *their*
 * open, two ~simultaneous opens would stall each other until the internal timeout (a minute), then both
 * fail with the non-fatal timeout error.  The impl prevents this (Server_session_impl::open_channel()
 * explains how); tight loops on both sides cross opens essentially every iteration, so a regression
 * surfaces reliably as a long stall + failure rather than sporadically.
 *
 * Asserted: every open_channel() succeeds, and promptly (the bound is far below the internal timeout yet
 * generous versus a healthy open, which takes milliseconds even with sanitizers); the passive side of
 * each session sees exactly the expected number of channels; the session is intact afterwards.  A side
 * stops looping at its first failed or slow open, so a regressed run costs about one stall, not N of them.
 *
 * Channel ends are not accumulated: a channel end costs on the order of 25 descriptors in this config (POSIX MQ +
 * handles: each MQ handle alone has its MQ descriptor, 2 epoll descriptors and 2 interrupter pipes; each MQ
 * pipe-end adds a timer pipe and a ready pipe; the socket stream adds itself and a timer pipe), and both ends
 * live in this process; holding a few dozen channels would exhaust the default per-process descriptor limit.
 * Each active end dies right after its open returns.  Each passive side, though, keeps its *latest* end until
 * the next one arrives (or until the end of the test).  Reason: destroying either MQ pipe-end unlinks the MQ's
 * name, and the passive side gets its end (and may destroy it) right after the response is sent -- so
 * destroying it immediately races the opposing active side's attaching to that very MQ by name; losing the
 * race yields a channel-creation error over there.  Keeping the latest end removes the race deterministically:
 * the passive handler for open k+1 runs only after the opposing side's open_channel() for open k has returned,
 * meaning that side has long since attached.
 *
 * Under TSAN the test is skipped.  It creates and destroys channel ends -- hence descriptors -- on 4 threads in
 * quick succession, and TSAN tracks synchronization per descriptor *number*: a close() on one thread followed by
 * the kernel handing the same number to another thread's new descriptor reads to TSAN as a data race between two
 * unrelated objects.  Those false positives are suppressed (test/suite/unit_test/sanitize/tsan/), but clang-15's
 * TSAN runtime crashes with an internal check failure while merely *composing* such a report, before any
 * suppression can apply.  The regression this test guards (cross-process deadlock) has nothing to do with data
 * races and is covered by the non-TSAN cells of any reasonable pipeline test matrix (starting with/including
 * our GitHub-CI workflow). */
TYPED_TEST_P(Session_connect_test, Open_channel_crossing)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  if constexpr(flow::test::tsan_enabled())
  {
    GTEST_SKIP() << "Skipped under ThreadSanitizer: descriptor-number-reuse false positives (fatal to clang-15's "
                    "TSAN runtime); see the test's doc comment.";
  }
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;
  using flow::Fine_clock;
  using boost::chrono::seconds;
  using boost::chrono::milliseconds;
  using boost::chrono::round;
  using flow::util::Mutex_non_recursive;
  using flow::util::Lock_guard;
  using std::atomic;

  constexpr size_t N_OPENS_PER_SIDE = 20;
  const seconds MAX_OPEN_DURATION{10}; // Versus the internal open-channel timeout: 60 s.

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("OpenCross");
  pair.remove_server_persistent_bits(false);

  // Passive-open bookkeeping per side.  The handler runs on the session's internal thread; hence shared_ptr + sync.
  struct Passive_side
  {
    atomic<size_t> m_n_chans{0};
    boost::promise<void> m_all_arrived; // Fulfilled once m_n_chans reaches N_OPENS_PER_SIDE.
    Mutex_non_recursive m_mutex;
    Channel_obj m_latest_chan; // Protected by m_mutex.  See the doc comment above the test for why this is kept.
  };
  const auto srv_passive = boost::make_shared<Passive_side>();
  const auto cli_passive = boost::make_shared<Passive_side>();
  const auto make_passive_handler = [](const boost::shared_ptr<Passive_side>& side)
  {
    return [side](Channel_obj&& new_chan, auto&& /*mdt_reader*/)
    {
      {
        Lock_guard<Mutex_non_recursive> lock(side->m_mutex);
        side->m_latest_chan = std::move(new_chan); // The previous latest end dies here.
      }
      if ((++side->m_n_chans) == N_OPENS_PER_SIDE)
      {
        side->m_all_arrived.set_value();
      }
    };
  };

  this->start_server(&pair);
  Server_session srv_session;
  const auto outcome = this->post_accept(&pair, &srv_session);
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {},
                     make_passive_handler(cli_passive)};
  Error_code err_code;
  EXPECT_TRUE(cli.sync_connect(cli.mdt_builder(), nullptr, nullptr, nullptr, &err_code));
  EXPECT_FALSE(err_code) << "sync_connect error: [" << err_code << "] [" << err_code.message() << "].";
  Error_code accept_err;
  this->await_accept(outcome, &accept_err);
  ASSERT_FALSE(accept_err) << "async_accept error: [" << accept_err << "] [" << accept_err.message() << "].";
  srv_session.init_handlers([](const Error_code&) {}, make_passive_handler(srv_passive));

  // The onslaught proper.  Each side stops at the first failed or slow open, recording why.
  struct Active_side
  {
    size_t m_n_chans = 0;
    string m_failure; // Empty if all went well.
  };
  const auto onslaught = [&](auto* session, Active_side* side, const string& ctx)
  {
    for (size_t idx = 0; idx != N_OPENS_PER_SIDE; ++idx)
    {
      Channel_obj chan; // Dies at iteration end.
      Error_code chan_err;
      const auto start = Fine_clock::now();
      const bool ok = session->open_channel(&chan, &chan_err);
      const auto duration = Fine_clock::now() - start;
      if ((!ok) || chan_err || (duration > MAX_OPEN_DURATION))
      {
        side->m_failure = ostream_op_string(ctx, ": open #", idx, ": carried-out=[", ok, "]; error=[", chan_err,
                                            "] [", chan_err.message(), "]; duration=[",
                                            round<milliseconds>(duration), "].");
        return;
      }
      // else
      ++side->m_n_chans;
    }
  };

  FLOW_LOG_INFO("Session up; both sides passive-open-capable.  Each side actively opens [" << N_OPENS_PER_SIDE << "] "
                "channels in a tight loop, concurrently: the client on a helper thread, the server here.");
  Active_side srv_active;
  Active_side cli_active;
  {
    Single_thread_task_loop cli_thread{nullptr, "oc_cli"};
    cli_thread.start();
    boost::promise<void> cli_done;
    cli_thread.post([&]()
    {
      onslaught(&cli, &cli_active, "cli");
      cli_done.set_value();
    });
    onslaught(&srv_session, &srv_active, "srv");
    cli_done.get_future().wait(); // Unconditional (no ASSERT bail-out until here): the task references our locals.
  }

  EXPECT_TRUE(srv_active.m_failure.empty()) << srv_active.m_failure;
  EXPECT_TRUE(cli_active.m_failure.empty()) << cli_active.m_failure;
  EXPECT_EQ(srv_active.m_n_chans, N_OPENS_PER_SIDE);
  EXPECT_EQ(cli_active.m_n_chans, N_OPENS_PER_SIDE);

  /* Each successful active open has a passive counterpart on the other side, delivered to the handler at
   * some point after the response is sent; so a short wait is appropriate. */
  const auto check_passive = [&](Passive_side* side, const string& ctx)
  {
    EXPECT_EQ(side->m_all_arrived.get_future().wait_for(seconds(5)), boost::future_status::ready)
      << ctx << ": not all passive-opens arrived in time.";
    EXPECT_EQ(side->m_n_chans.load(), N_OPENS_PER_SIDE) << ctx;
  };
  check_passive(srv_passive.get(), "srv");
  check_passive(cli_passive.get(), "cli");

  // No worse for wear?
  EXPECT_FALSE(srv_session.session_token().is_nil());
  EXPECT_EQ(srv_session.session_token(), cli.session_token());

  // The remaining channel ends predecease the sessions.
  for (auto* side : { srv_passive.get(), cli_passive.get() })
  {
    Lock_guard<Mutex_non_recursive> lock(side->m_mutex);
    side->m_latest_chan = Channel_obj{};
  }

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
} // TYPED_TEST_P(Session_connect_test, Open_channel_crossing)

/* The server-side user closes each just-passive-opened channel end immediately; the client actively opens.
 * See run_peer_closes_immediately() for the scenario, the race, and what is asserted. */
TYPED_TEST_P(Session_connect_test, Open_channel_peer_closes_immediately)
{
  this->run_peer_closes_immediately(false);
}

/* As Open_channel_peer_closes_immediately, mirrored: the client-side user closes; the server actively opens.
 * Today this direction has no race (see run_peer_closes_immediately()); the test deliberately does not rely on
 * that. */
TYPED_TEST_P(Session_connect_test, Open_channel_peer_closes_immediately_srv_opens)
{
  this->run_peer_closes_immediately(true);
}

/* A pending async_accept() aborted by Session_server destruction: per the boost.asio-like contract the
 * handler must still fire -- with the specific object-shutdown code. */
TYPED_TEST_P(Session_connect_test, Accept_abort)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Server_session = typename TestFixture::Server_session;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("AcceptAbort");
  pair.remove_server_persistent_bits(false);

  this->start_server(&pair);
  Server_session srv_session;
  const auto outcome = this->post_accept(&pair, &srv_session);

  FLOW_LOG_INFO("Destroying the listening server with the async_accept() outstanding; no client ever came.");
  pair.m_srv.reset();
  Error_code accept_err;
  this->await_accept(outcome, &accept_err);
  EXPECT_TRUE(accept_err == error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
    << "Expected the object-shutdown code; got: [" << accept_err << "] [" << accept_err.message() << "].";

  pair.remove_server_persistent_bits(); // The server did run (briefly): the CNS file + mutex exist.
}

/* Session_server destroyed while session-opens are in flight: a regression test for its shutdown design (see
 * Session_server_impl::dtor_stop_accepting()).  Each round: a fresh server; N_ACCEPTS async_accept()s posted;
 * N_CLIENTS clients -- more than N_ACCEPTS, so the extras sit in the socket-acceptor's surplus queue -- each
 * sync_connect() concurrently on its own thread; after a round-specific delay the server is destroyed.  Depending
 * on timing that lands before any connect, among the socket-accepts, mid-log-in, or after some accepts completed.
 * To spread the rounds over those regions regardless of the session type's log-in speed (or of a sanitizer's
 * slowdown), the delays are not fixed: round 0 calibrates -- it lets all accepts complete, measuring how long that
 * takes -- and each later round's delay is a fraction of that, from 0 to past 1.  Asserted per round:
 *   - Each async_accept() handler fires exactly once; its code is success or the object-shutdown code (the dtor
 *     contract's code for pending ones) -- nothing else.
 *   - Every client's sync_connect() returns (success or error: timing-dependent, not asserted).
 *   - Sessions emitted before the destruction outlive the server fine; they are destroyed afterwards (allowed per
 *     the lifetime tenets in the Session_server doc header: such a session merely needs destroying).
 * The detectors for the actual subject -- races between the dtor and in-flight log-in work, which reaches all 3
 * server tiers (the base core; and SHM-provider sub-class state, via per-app setup and app_shm()) -- are, beyond
 * crashes and asserts, the ASAN and TSAN pipeline cells.  Outcome tallies are logged for information.
 *
 * Also covered, as a by-product: each round's Session_server is a new one for the same Server_app, in the same
 * process, constructed after the previous round's sessions and clients are all gone -- the allowed form of
 * replacing a Session_server (tenet 3 in that doc header).
 *
 * Lifetimes: each accept's target Server_session is owned by the test body only (handlers capture a separate
 * outcome holder), so no session can be destroyed on its own thread by a handler capture's release.  Server sides
 * are destroyed before client sides: never init_handlers()ed, they have no SHM-jemalloc dtor gate; and each
 * client, its peer gone, has no gate to wait on either. */
TYPED_TEST_P(Session_connect_test, Server_destroyed_mid_flight)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using flow::async::Single_thread_task_loop;
  using flow::async::Synchronicity;
  using flow::util::ostream_op_string;
  using flow::Fine_clock;
  using flow::Fine_duration;
  using boost::chrono::seconds;
  using boost::chrono::milliseconds;
  using boost::chrono::microseconds;
  using boost::chrono::round;
  using std::atomic;
  using std::vector;

  constexpr size_t N_ACCEPTS = 3;
  constexpr size_t N_CLIENTS = 4;
  constexpr size_t N_ROUNDS = 16; // Round 0 = calibration; the rest cycle through DELAY_FRACTIONS.
  constexpr double DELAY_FRACTIONS[] = { 0, 0.05, 0.1, 0.2, 0.35, 0.5, 0.7, 0.9, 1.1 }; // Of the calibrated duration.
  constexpr size_t N_DELAY_FRACTIONS = sizeof(DELAY_FRACTIONS) / sizeof(DELAY_FRACTIONS[0]);

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("MidFlight");
  pair.remove_server_persistent_bits(false);

  // One thread per client, reused across rounds.
  vector<std::unique_ptr<Single_thread_task_loop>> cli_threads;
  for (size_t idx = 0; idx != N_CLIENTS; ++idx)
  {
    cli_threads.emplace_back(std::make_unique<Single_thread_task_loop>(nullptr, ostream_op_string("midFl", idx)));
    cli_threads.back()->start();
  }

  struct Accept_outcome_slot
  {
    atomic<int> m_n_fired{0};
    Error_code m_err; // Written before m_n_fired is incremented.
  };
  struct Cli_slot
  {
    Client_session m_cli;
    bool m_ok = false;
    Error_code m_err;
    boost::promise<void> m_done;
  };

  size_t n_accept_ok = 0;
  size_t n_accept_aborted = 0;
  size_t n_cli_ok = 0;
  size_t n_cli_err = 0;
  Fine_duration calib_duration{}; // Round 0 measures it: clients launched => all N_ACCEPTS accepts completed.

  for (size_t round_idx = 0; round_idx != N_ROUNDS; ++round_idx)
  {
    this->start_server(&pair);

    vector<Server_session> srv_sessions(N_ACCEPTS); // Sized up front: the accepts hold pointers into it.
    vector<boost::shared_ptr<Accept_outcome_slot>> accept_outcomes;
    for (auto& srv_session : srv_sessions)
    {
      const auto outcome = boost::make_shared<Accept_outcome_slot>();
      accept_outcomes.push_back(outcome);
      pair.m_srv->async_accept(&srv_session, [outcome](const Error_code& err_code)
      {
        outcome->m_err = err_code;
        ++outcome->m_n_fired;
      });
    }
    const auto all_fired = [&]()
    {
      for (const auto& outcome : accept_outcomes)
      {
        if (outcome->m_n_fired.load() == 0)
        {
          return false;
        }
      }
      return true;
    };

    vector<boost::shared_ptr<Cli_slot>> clis;
    for (size_t idx = 0; idx != N_CLIENTS; ++idx)
    {
      const auto slot = boost::make_shared<Cli_slot>();
      slot->m_cli = Client_session{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
      clis.push_back(slot);
      cli_threads[idx]->post([slot]()
      {
        slot->m_ok = slot->m_cli.sync_connect(slot->m_cli.mdt_builder(), nullptr, nullptr, nullptr, &slot->m_err);
        slot->m_done.set_value();
      });
    }

    if (round_idx == 0)
    {
      // Calibration round: let everything complete, timing it; the server is destroyed only after that.
      const auto start = Fine_clock::now();
      for (size_t idx = 0; (idx != 10000) && (!all_fired()); ++idx)
      {
        flow::util::this_thread::sleep_for(milliseconds(1));
      }
      ASSERT_TRUE(all_fired()) << "Calibration round: not all accepts completed in time.";
      calib_duration = Fine_clock::now() - start;
      FLOW_LOG_INFO("Calibration round: [" << N_ACCEPTS << "] accepts completed in "
                    "[" << round<microseconds>(calib_duration) << "]; later rounds destroy the server after "
                    "fractions of that.");
    }
    else
    {
      flow::util::this_thread::sleep_for
        (round<microseconds>(calib_duration * DELAY_FRACTIONS[(round_idx - 1) % N_DELAY_FRACTIONS]));
    }
    pair.m_srv.reset(); // The subject.

    for (size_t idx = 0; idx != N_CLIENTS; ++idx)
    {
      auto& slot = *(clis[idx]);
      ASSERT_EQ(slot.m_done.get_future().wait_for(seconds(10)), boost::future_status::ready)
        << "Round [" << round_idx << "]: client [" << idx << "]'s sync_connect() did not return.";
      EXPECT_TRUE(slot.m_ok) << "Round [" << round_idx << "]: client [" << idx << "].";
      if (slot.m_err)
      {
        ++n_cli_err;
      }
      else
      {
        ++n_cli_ok;
      }
      // Flush: the task (and its capture) is gone after this; so the Client_session dies below, on this thread.
      cli_threads[idx]->post([]() {}, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
    }

    /* Per the dtor contract pending handlers fire "ASAP" (in practice all have fired by the time the dtor returns);
     * allow a moment. */
    for (size_t idx = 0; (idx != 500) && (!all_fired()); ++idx)
    {
      flow::util::this_thread::sleep_for(milliseconds(10));
    }
    for (size_t idx = 0; idx != N_ACCEPTS; ++idx)
    {
      const auto& outcome = *(accept_outcomes[idx]);
      EXPECT_EQ(outcome.m_n_fired.load(), 1) << "Round [" << round_idx << "]: accept [" << idx << "] handler firings.";
      if (!outcome.m_err)
      {
        ++n_accept_ok;
      }
      else
      {
        EXPECT_TRUE(outcome.m_err == error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER)
          << "Round [" << round_idx << "]: accept [" << idx << "]: expected success or the object-shutdown code; got: "
             "[" << outcome.m_err << "] [" << outcome.m_err.message() << "].";
        ++n_accept_aborted;
      }
    }

    // The emitted sessions outlived their server; now destroy them: server sides first (see doc comment above).
    srv_sessions.clear();
    clis.clear();
    pair.remove_server_persistent_bits();
  } // for (round_idx)

  FLOW_LOG_INFO("Over [" << N_ROUNDS << "] rounds (calibrated log-in duration for [" << N_ACCEPTS << "] accepts: "
                "[" << round<microseconds>(calib_duration) << "]): accepts: [" << n_accept_ok << "] succeeded, "
                "[" << n_accept_aborted << "] aborted by server destruction; client connects: [" << n_cli_ok << "] "
                "succeeded, [" << n_cli_err << "] failed.");
} // TYPED_TEST_P(Session_connect_test, Server_destroyed_mid_flight)

/* A used session ends while another session-open, for the same Client_app, is in flight against the same
 * Session_server.  This is the in-process form of a situation that transport_test exercise-mode creates across 2
 * processes: the client ends its session and immediately reconnects; the server, having re-armed async_accept() right
 * after the first accept, destroys its side of the ended session while the new log-in proceeds.  Nothing else is
 * shut down: the Session_server and all threads persist across rounds.
 *
 * Each round:
 *   - Session A is established with 2 init-channels.  B's async_accept() is posted at once (it stays outstanding).
 *   - Each A channel, on each side, is upgraded to a struc::Channel: for the SHM types SHM-backed -- channel 0 via
 *     the session-scope arena, channel 1 via the app-scope one (on a side that has one; else session-scope); for
 *     vanilla heap-backed.  Then, on each channel, each side async_request()s once and answers the opposing side's
 *     request: so 2 requests travel in each direction and are answered.  (For the SHM types this allocates in the
 *     arenas on both sides, so A's teardown has real SHM state to dismantle.)
 *   - A's client side (channels, then session) is destroyed, on a helper thread (for SHM-jemalloc its dtor blocks
 *     until the opposing dtor begins); A's server side observes this via its error handler.
 *   - B's client connect starts (on its own thread); after a round-specific delay A's server side (channels, then
 *     session) is destroyed.  As in Server_destroyed_mid_flight the delays are fractions of a duration calibrated in
 *     round 0: there, B's log-in duration (connect start => accept completion), with A's server side destroyed only
 *     after that.
 * Asserted per round: each request is answered with the expected value; A's server-side error handler fires exactly
 * once; A's client-side dtor completes; B's connect and accept both succeed, the two B sides agree on the session
 * token; and, for the SHM types, B's app-scope arena (the app being the same as A's) is usable: an object constructed
 * in it round-trips via lend/borrow.  Beyond these, the detectors are crashes, asserts, and the ASAN/TSAN cells.
 *
 * Under TSAN the SHM-classic variant is skipped.  There, objects in a pool are shared-owned by the two sides, each of
 * which maps the pool at its own address; so a chunk freed via one side's mapping (and its address of the in-SHM
 * allocator mutex) and then allocated anew via the other's looks, to TSAN, like a race.  These false positives are
 * reliable once struc::Channel traffic flows over SHM-classic (transport_test's SHM-classic TSAN suppressions
 * concern the same phenomenon).  The other two variants run under TSAN as normal. */
TYPED_TEST_P(Session_connect_test, Session_end_during_connect)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;

  if constexpr(flow::test::tsan_enabled() && (Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::CLASSIC))
  {
    GTEST_SKIP() << "Skipped under ThreadSanitizer for SHM-classic: cross-mapping SHM false positives; see the test's "
                    "doc comment.";
  }
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;
  using Channels = typename Pair::Channels;
  using Body = transport::struc::test::Body;
  using Struc_channel = typename Client_session::template Structured_channel<Body>;
  using transport::struc::Channel_base;
  using flow::async::Single_thread_task_loop;
  using flow::async::Synchronicity;
  using flow::Fine_clock;
  using flow::Fine_duration;
  using boost::chrono::seconds;
  using boost::chrono::microseconds;
  using boost::chrono::round;
  using std::atomic;
  using std::vector;
  using std::unique_ptr;

  constexpr size_t N_CHANNELS = 2;
  constexpr int N_RSPS = 2 * N_CHANNELS; // 1 request per channel per direction.
  constexpr double DELAY_FRACTIONS[] = { 0, 0.05, 0.1, 0.2, 0.35, 0.5, 0.7, 0.9, 1.1 }; // Of the calibrated duration.
  constexpr size_t N_DELAY_FRACTIONS = sizeof(DELAY_FRACTIONS) / sizeof(DELAY_FRACTIONS[0]);
  constexpr size_t N_ROUNDS = 1 + (2 * N_DELAY_FRACTIONS); // Round 0 = calibration; then each fraction twice.

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("EndVsConnect");
  pair.remove_server_persistent_bits(false);
  this->start_server(&pair);

  Single_thread_task_loop cli_a_ender{nullptr, "endCnA"};
  cli_a_ender.start();
  Single_thread_task_loop cli_b_thread{nullptr, "endCnB"};
  cli_b_thread.start();

  // Upgrades a just-established A channel (on the side of `*session`) to a started struc::Channel; see doc above.
  const auto upgrade = [&](auto* session, Channel_obj* chan, bool app_scope) -> unique_ptr<Struc_channel>
  {
    using Session_t = std::remove_pointer_t<decltype(session)>;

    unique_ptr<Struc_channel> struc;
    if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::NONE)
    {
      struc = std::make_unique<Struc_channel>(this->ipc_logger(), std::move(*chan),
                                              Channel_base::S_SERIALIZE_VIA_HEAP, session->session_token());
    }
    else
    {
      if constexpr(Has_app_shm<Session_t>::value)
      {
        if (app_scope)
        {
          struc = std::make_unique<Struc_channel>(this->ipc_logger(), std::move(*chan),
                                                  Channel_base::S_SERIALIZE_VIA_APP_SHM, session);
        }
      }
      if (!struc)
      {
        struc = std::make_unique<Struc_channel>(this->ipc_logger(), std::move(*chan),
                                                Channel_base::S_SERIALIZE_VIA_SESSION_SHM, session);
      }
    }
    struc->start([](const Error_code&) {}); // Fires when the opposing side goes away; not our subject.
    return struc;
  };

  struct Exchange
  {
    atomic<int> m_n_rsps{0};
    atomic<int> m_n_bad{0}; // Responses with an unexpected body.
    boost::promise<void> m_done; // Set upon the N_RSPS-th response.
  };
  // On `*chan`: answer each incoming request (value + 1); send our own request (value `val`), expecting val + 1.
  const auto exchange_on = [&](Struc_channel* chan, uint64_t val, const boost::shared_ptr<Exchange>& ex)
  {
    EXPECT_TRUE(chan->expect_msgs(Body::COOL_REQ, [chan](auto&& req)
    {
      auto rsp = chan->create_msg();
      rsp.body_root()->initCoolRsp().setCoolVal(req->body_root().getCoolReq().getCoolVal() + 1);
      chan->send(&rsp, req.get());
    }));

    auto req = chan->create_msg();
    req.body_root()->initCoolReq().setCoolVal(val);
    EXPECT_TRUE(chan->async_request(&req, nullptr, nullptr, [ex, val](auto&& rsp)
    {
      const auto body = rsp->body_root();
      if ((body.which() != Body::COOL_RSP) || (body.getCoolRsp().getCoolVal() != (val + 1)))
      {
        ++ex->m_n_bad;
      }
      if (++ex->m_n_rsps == N_RSPS)
      {
        ex->m_done.set_value();
      }
    }));
  };

  struct Cli_slot
  {
    Client_session m_cli;
    bool m_ok = false;
    Error_code m_err;
    boost::promise<void> m_done;
  };

  Fine_duration calib_duration{}; // Round 0 measures it: B's connect start => B's accept completion.

  for (size_t round_idx = 0; round_idx != N_ROUNDS; ++round_idx)
  {
    const auto ctx = flow::util::ostream_op_string("Round [", round_idx, "]: ");

    // Session A, with its server side's error handler ours; then B's accept, outstanding from here on.
    const auto cli_a = boost::make_shared<Client_session>(); // Shared with cli_a_ender's task.
    Server_session srv_a;
    Channels cli_a_chans;
    Channels srv_a_chans;
    pair.connect_sessions(this->ipc_logger(), cli_a.get(), &srv_a, N_CHANNELS, &cli_a_chans, &srv_a_chans, true);
    const auto srv_a_n_hosed = boost::make_shared<atomic<int>>(0);
    const auto srv_a_hosed = boost::make_shared<boost::promise<void>>();
    srv_a.init_handlers([srv_a_n_hosed, srv_a_hosed](const Error_code&)
    {
      if (srv_a_n_hosed->fetch_add(1) == 0)
      {
        srv_a_hosed->set_value();
      }
    });
    if ((cli_a_chans.size() != N_CHANNELS) || (srv_a_chans.size() != N_CHANNELS))
    {
      ADD_FAILURE() << ctx << "Session A init-channel count mismatch; bailing.";
      pair.destroy_sessions(cli_a.get(), &srv_a);
      break; // (Before B's accept is posted: the server shall not be left targeting a dead Server_session.)
    }

    Server_session srv_b;
    const auto accept_b = this->post_accept(&pair, &srv_b);
    auto accept_b_done = accept_b.m_done->get_future();

    // Light use of A.
    vector<unique_ptr<Struc_channel>> cli_a_strucs;
    vector<unique_ptr<Struc_channel>> srv_a_strucs;
    for (size_t idx = 0; idx != N_CHANNELS; ++idx)
    {
      cli_a_strucs.emplace_back(upgrade(cli_a.get(), &cli_a_chans[idx], idx == 1));
      srv_a_strucs.emplace_back(upgrade(&srv_a, &srv_a_chans[idx], idx == 1));
    }
    {
      const auto ex = boost::make_shared<Exchange>();
      for (size_t idx = 0; idx != N_CHANNELS; ++idx)
      {
        exchange_on(cli_a_strucs[idx].get(), (round_idx * 100) + idx, ex);
        exchange_on(srv_a_strucs[idx].get(), (round_idx * 100) + 10 + idx, ex);
      }
      EXPECT_EQ(ex->m_done.get_future().wait_for(seconds(5)), boost::future_status::ready)
        << ctx << "Not all requests on session A were answered in time.";
      EXPECT_EQ(ex->m_n_bad.load(), 0) << ctx << "Unexpected response contents on session A.";
    }

    // A ends: client side first.
    cli_a_strucs.clear();
    cli_a_ender.post([cli_a]() { *cli_a = Client_session{}; });
    EXPECT_EQ(srv_a_hosed->get_future().wait_for(seconds(5)), boost::future_status::ready)
      << ctx << "Session A's server-side error handler did not fire though its client side was destroyed.";

    // B connects, while (after the round's delay) A's server side is destroyed: the subject.
    const auto cli_b = boost::make_shared<Cli_slot>();
    cli_b->m_cli = Client_session{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
    const auto start = Fine_clock::now();
    cli_b_thread.post([cli_b]()
    {
      cli_b->m_ok = cli_b->m_cli.sync_connect(cli_b->m_cli.mdt_builder(), nullptr, nullptr, nullptr, &cli_b->m_err);
      cli_b->m_done.set_value();
    });
    if (round_idx == 0)
    {
      ASSERT_EQ(accept_b_done.wait_for(seconds(5)), boost::future_status::ready)
        << ctx << "Calibration round: B's accept did not complete in time.";
      calib_duration = Fine_clock::now() - start;
      FLOW_LOG_INFO("Calibration round: B's log-in took [" << round<microseconds>(calib_duration) << "]; later "
                    "rounds destroy A's server side after fractions of that.");
    }
    else
    {
      flow::util::this_thread::sleep_for
        (round<microseconds>(calib_duration * DELAY_FRACTIONS[(round_idx - 1) % N_DELAY_FRACTIONS]));
    }
    srv_a_strucs.clear();
    srv_a = Server_session{};

    // A's client-side dtor (SHM-jemalloc: released by the server-side dtor's start) must now complete.
    cli_a_ender.post([]() {}, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION);
    EXPECT_EQ(srv_a_n_hosed->load(), 1) << ctx << "Session A's server-side error handler firings.";

    // B must be fine.
    ASSERT_EQ(cli_b->m_done.get_future().wait_for(seconds(10)), boost::future_status::ready)
      << ctx << "B's sync_connect() did not return.";
    cli_b_thread.post([]() {}, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_COMPLETION); // Release the task's capture.
    EXPECT_TRUE(cli_b->m_ok) << ctx;
    EXPECT_FALSE(cli_b->m_err) << ctx << "B's sync_connect() error: [" << cli_b->m_err << "] "
                                         "[" << cli_b->m_err.message() << "].";
    ASSERT_EQ(accept_b_done.wait_for(seconds(5)), boost::future_status::ready) << ctx << "B's accept did not complete.";
    EXPECT_FALSE(*accept_b.m_err) << ctx << "B's accept error: [" << *accept_b.m_err << "] "
                                            "[" << accept_b.m_err->message() << "].";
    srv_b.init_handlers([](const Error_code&) {});
    EXPECT_FALSE(srv_b.session_token().is_nil()) << ctx;
    EXPECT_EQ(srv_b.session_token(), cli_b->m_cli.session_token()) << ctx;

    if constexpr(Pair::S_SHM_TYPE_OR_NONE != schema::ShmType::NONE)
    {
      auto* const app_arena = srv_b.app_shm();
      EXPECT_NE(app_arena, nullptr) << ctx;
      if (app_arena)
      { // Scope: handles must be dropped before the sessions are destroyed.
        const auto obj = app_arena->template construct<int>(int(round_idx));
        EXPECT_TRUE(obj) << ctx;
        if (obj)
        {
          const auto blob = srv_b.template lend_object<int>(obj);
          EXPECT_FALSE(blob.empty()) << ctx;
          const auto borrowed = cli_b->m_cli.template borrow_object<int>(blob);
          EXPECT_TRUE(borrowed) << ctx;
          if (borrowed)
          {
            EXPECT_EQ(*borrowed, int(round_idx)) << ctx;
          }
        }
      }
    }

    pair.destroy_sessions(&cli_b->m_cli, &srv_b);
  } // for (round_idx)

  cli_a_ender.stop();
  cli_b_thread.stop();
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
} // TYPED_TEST_P(Session_connect_test, Session_end_during_connect)

/* Two async_accept()s outstanding simultaneously -- explicitly supported (each accept's handling is
 * independent per the impl design) -- then two clients connect: both accepts must complete, and the two
 * full sessions coexist.  Accept-to-connect pairing is FIFO in practice but deliberately not asserted;
 * the token checks below are pairing-agnostic (and then teardown pairs the objects up by token, as the
 * SHM-jemalloc dtors rendezvous with their actual opposing peers). */
TYPED_TEST_P(Session_connect_test, Concurrent_accepts)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("TwoAccepts");
  pair.remove_server_persistent_bits(false);

  this->start_server(&pair);
  Server_session srv_session_1;
  Server_session srv_session_2;
  const auto outcome_1 = this->post_accept(&pair, &srv_session_1);
  const auto outcome_2 = this->post_accept(&pair, &srv_session_2);

  FLOW_LOG_INFO("2 async_accept()s outstanding; 2 clients connect.");
  Client_session cli_1{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Client_session cli_2{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Error_code err_code;
  EXPECT_TRUE(cli_1.sync_connect(cli_1.mdt_builder(), nullptr, nullptr, nullptr, &err_code));
  EXPECT_FALSE(err_code);
  EXPECT_TRUE(cli_2.sync_connect(cli_2.mdt_builder(), nullptr, nullptr, nullptr, &err_code));
  EXPECT_FALSE(err_code);

  Error_code accept_err;
  this->await_accept(outcome_1, &accept_err);
  EXPECT_FALSE(accept_err);
  this->await_accept(outcome_2, &accept_err);
  EXPECT_FALSE(accept_err);
  srv_session_1.init_handlers([](const Error_code&) {});
  srv_session_2.init_handlers([](const Error_code&) {});

  // 2 live sessions; distinct; each client paired with exactly one server session (whichever it is).
  const auto tok_c1 = cli_1.session_token();
  const auto tok_c2 = cli_2.session_token();
  const auto tok_s1 = srv_session_1.session_token();
  const auto tok_s2 = srv_session_2.session_token();
  EXPECT_FALSE(tok_c1.is_nil());
  EXPECT_FALSE(tok_c2.is_nil());
  EXPECT_NE(tok_c1, tok_c2);
  EXPECT_TRUE(((tok_c1 == tok_s1) && (tok_c2 == tok_s2)) || ((tok_c1 == tok_s2) && (tok_c2 == tok_s1)));

  // Tear down in true pairs.
  auto* const srv_for_cli_1 = (tok_c1 == tok_s1) ? &srv_session_1 : &srv_session_2;
  auto* const srv_for_cli_2 = (srv_for_cli_1 == &srv_session_1) ? &srv_session_2 : &srv_session_1;
  pair.destroy_sessions(&cli_1, srv_for_cli_1);
  pair.destroy_sessions(&cli_2, srv_for_cli_2);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* The `sync_io`-pattern server-side adapters (Session_server_adapter, Server_session_adapter): their API-misuse
 * guards must refuse (return false; no-op) rather than misbehave.  In order:
 *   - Session_server_adapter::async_accept() before its start_ops(): refused; nothing is started (its handler
 *     never fires).
 *   - Server_session_adapter::init_handlers() before its start_ops(): refused.
 *   - Ditto after start_ops(), but before the adapter holds an accepted session: refused -- and without residue:
 *     once a successful async_accept() fills that same adapter, init_handlers() succeeds.
 *   - A second async_accept() while one is outstanding: refused (the outstanding one completes normally).
 *   - init_handlers() again in PEER state: refused; ditto after the session's error handler has fired (the
 *     opposing client session having been destroyed).
 * The client side is a regular async-I/O Client_session: the adapters' dealings with it are not the subject.
 * All adapter API calls run on the event-loop thread, since the `sync_io` pattern forbids them to run
 * concurrently with the adapters' `(*on_active_ev_func)()` calls (which run there). */
TYPED_TEST_P(Session_connect_test, Sync_io_adapter_guards)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Session_server_sio = typename TestFixture::Session_server::Sync_io_obj;
  using Server_session_sio = typename Session_server_sio::Session_obj;
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;
  using flow::async::Single_thread_task_loop;
  using boost::chrono::seconds;
  using std::atomic;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("SioGuards");
  pair.remove_server_persistent_bits(false);

  /* The user event loop.  Teardown (at the end) stops it before destroying the adapters, whose event-wait
   * handles are its I/O objects.  (On an ASSERT bail-out the adapters die first, with the loop idling in its
   * reactor -- acceptable for a failing run.)  ender_thread: see the client-destruction step. */
  Single_thread_task_loop loop{nullptr, "sioGuards"};
  loop.start();
  Single_thread_task_loop ender_thread{nullptr, "sioEnder"};
  ender_thread.start();

  // Runs `func` on the loop thread; returns once it has.
  const auto on_loop = [&](const auto& func)
  {
    boost::promise<void> done;
    loop.post([&]()
    {
      func();
      done.set_value();
    });
    done.get_future().wait();
  };

  // The canonical `sync_io`-pattern hookup: satisfy the adapters' async-wait requests via `loop`.
  const auto make_ev_hndl = [&loop]() { return Asio_waitable_native_handle{*(loop.task_engine())}; };
  const auto ev_wait_func = [](Asio_waitable_native_handle* hndl_of_interest,
                               bool ev_of_interest_snd_else_rcv, Task_ptr&& on_active_ev_func)
  {
    hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv ? Asio_waitable_native_handle::Base::wait_write
                                                             : Asio_waitable_native_handle::Base::wait_read,
                                 [on_active_ev_func = std::move(on_active_ev_func)](const Error_code& err_code)
    {
      if (err_code != boost::asio::error::operation_aborted)
      {
        (*on_active_ev_func)();
      }
    });
  };

  auto srv = std::make_unique<Session_server_sio>(this->ipc_logger(),
                                                  pair.m_srv_apps.find(pair.m_srv_app.m_name)->second,
                                                  pair.m_cli_apps);
  if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::CLASSIC)
  {
    srv->core()->pool_size_limit_mi(TestFixture::S_SHM_CLASSIC_POOL_SIZE_LIMIT_MI); // See start_server().
  }
  auto sess = std::make_unique<Server_session_sio>();

  // Handler outcome holders; shared_ptr for the usual reason (no handler may fire into dead locals).
  const auto accept_done = boost::make_shared<boost::promise<void>>();
  const auto accept_err = boost::make_shared<Error_code>();
  const auto on_accept = [accept_done, accept_err](const Error_code& err_code)
  {
    *accept_err = err_code;
    accept_done->set_value();
  };
  const auto stray_fired = boost::make_shared<atomic<bool>>(false);
  const auto on_stray_accept = [stray_fired](const Error_code&) { *stray_fired = true; };
  const auto hosed = boost::make_shared<boost::promise<void>>();
  const auto on_err = [hosed](const Error_code&) { hosed->set_value(); };

  FLOW_LOG_INFO("Before any start_ops(): async_accept() and init_handlers() must be refused.");
  on_loop([&]()
  {
    EXPECT_FALSE(srv->async_accept(sess.get(), on_stray_accept));
    EXPECT_FALSE(sess->init_handlers(on_err));
  });

  FLOW_LOG_INFO("Both adapters started; init_handlers() on the not-yet-accepted session adapter must be refused.");
  on_loop([&]()
  {
    EXPECT_TRUE(srv->replace_event_wait_handles(make_ev_hndl));
    EXPECT_TRUE(srv->start_ops(ev_wait_func));
    EXPECT_TRUE(sess->replace_event_wait_handles(make_ev_hndl));
    EXPECT_TRUE(sess->start_ops(ev_wait_func));
    EXPECT_FALSE(sess->init_handlers(on_err));
  });

  FLOW_LOG_INFO("async_accept(); a second one while it is outstanding must be refused; then a client connects.");
  on_loop([&]()
  {
    EXPECT_TRUE(srv->async_accept(sess.get(), on_accept));
    EXPECT_FALSE(srv->async_accept(sess.get(), on_stray_accept));
  });
  const auto cli = boost::make_shared<Client_session>(this->ipc_logger(), pair.m_cli_app, pair.m_srv_app,
                                                      [](const Error_code&) {});
  Error_code err_code;
  EXPECT_TRUE(cli->sync_connect(cli->mdt_builder(), nullptr, nullptr, nullptr, &err_code));
  EXPECT_FALSE(err_code) << "sync_connect error: [" << err_code << "] [" << err_code.message() << "].";
  ASSERT_EQ(accept_done->get_future().wait_for(seconds(5)), boost::future_status::ready)
    << "Adapter async_accept handler did not fire in time.";
  EXPECT_FALSE(*accept_err) << "[" << *accept_err << "] [" << accept_err->message() << "].";

  FLOW_LOG_INFO("Accepted.  init_handlers() must now succeed (the earlier refusals left no residue); "
                "a repeat must be refused.");
  on_loop([&]()
  {
    EXPECT_TRUE(sess->init_handlers(on_err));
    EXPECT_FALSE(sess->init_handlers(on_err));
    EXPECT_FALSE(sess->core()->session_token().is_nil());
  });

  /* The client session's dtor runs on a helper thread: under SHM-jemalloc it blocks until the opposing (our
   * server session's) dtor begins -- see run_graceful_end().  Our dtor is at teardown below. */
  FLOW_LOG_INFO("Client session destroyed; the server session's error handler must fire; init_handlers() must "
                "still be refused.");
  ender_thread.post([cli]() { *cli = Client_session{}; });
  ASSERT_EQ(hosed->get_future().wait_for(seconds(5)), boost::future_status::ready)
    << "Server session adapter's error handler did not fire though the opposing session's dtor started.";
  on_loop([&]() { EXPECT_FALSE(sess->init_handlers(on_err)); });

  loop.stop();
  sess.reset(); // (SHM-jemalloc: this releases the client dtor, blocked on ender_thread.)
  ender_thread.stop();
  srv.reset();
  EXPECT_FALSE(stray_fired->load()) << "A refused async_accept()'s handler must never fire.";

  pair.remove_server_persistent_bits();
} // TYPED_TEST_P(Session_connect_test, Sync_io_adapter_guards)

/* Session_server::mq_msg_size_limit() is fixed per session, as of that session's async_accept(): a change after a
 * session was accepted applies to later-accepted sessions only -- not to channels the earlier session opens
 * afterwards.  (Regression aspect: a session must never query its Session_server for the value at channel-open time;
 * that would also make a session outliving its Session_server a use-after-free -- see Session_outlives_server.)
 * Two sessions, accepted at limits A then B; then each actively opens a channel (the server side creates the MQs, so
 * its per-session value is what counts), whose ends on both sides report the MQ max message size. */
TYPED_TEST_P(Session_connect_test, Mq_msg_size_limit_per_session)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;
  using boost::chrono::seconds;

  // Both max-alignment multiples: set-and-read-back exact (see Server_knobs for the rounding behavior).
  constexpr size_t LIMIT_A = 2048;
  constexpr size_t LIMIT_B = 4096;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("MqLimit");
  pair.remove_server_persistent_bits(false);
  this->start_server(&pair);

  // Passive-open-capable clients: the server sessions actively open; each client's handler loads its slot.
  struct Passive_open_slot
  {
    boost::promise<void> m_opened;
    Channel_obj m_chan;
  };
  const auto make_client = [&](const boost::shared_ptr<Passive_open_slot>& slot)
  {
    return Client_session{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {},
                          [slot](Channel_obj&& new_chan, auto&& /*mdt_reader*/)
    {
      slot->m_chan = std::move(new_chan);
      slot->m_opened.set_value();
    }};
  };
  const auto slot_a = boost::make_shared<Passive_open_slot>();
  const auto slot_b = boost::make_shared<Passive_open_slot>();

  pair.m_srv->mq_msg_size_limit(LIMIT_A);
  ASSERT_EQ(pair.m_srv->mq_msg_size_limit(), LIMIT_A);
  auto cli_a = make_client(slot_a);
  Server_session srv_session_a;
  this->connect_ok(&pair, &cli_a, &srv_session_a);

  pair.m_srv->mq_msg_size_limit(LIMIT_B);
  ASSERT_EQ(pair.m_srv->mq_msg_size_limit(), LIMIT_B);
  auto cli_b = make_client(slot_b);
  Server_session srv_session_b;
  this->connect_ok(&pair, &cli_b, &srv_session_b);

  FLOW_LOG_INFO("Session A accepted at limit [" << LIMIT_A << "], then session B at [" << LIMIT_B << "]; now each "
                "opens a channel: A's must use A's limit despite the later change.");
  const auto open_and_check = [&](Server_session& srv_session, const boost::shared_ptr<Passive_open_slot>& slot,
                                  size_t expected_limit, const string& ctx)
  {
    Channel_obj chan;
    Error_code chan_err;
    ASSERT_TRUE(srv_session.open_channel(&chan, &chan_err)) << ctx;
    ASSERT_FALSE(chan_err) << ctx << ": [" << chan_err << "] [" << chan_err.message() << "].";
    ASSERT_EQ(slot->m_opened.get_future().wait_for(seconds(5)), boost::future_status::ready) << ctx;
    EXPECT_EQ(chan.send_blob_max_size(), expected_limit) << ctx << " (server end).";
    EXPECT_EQ(slot->m_chan.send_blob_max_size(), expected_limit) << ctx << " (client end).";
    slot->m_chan = Channel_obj{}; // Channel ends predecease the sessions.
  };
  open_and_check(srv_session_a, slot_a, LIMIT_A, "session A");
  open_and_check(srv_session_b, slot_b, LIMIT_B, "session B");

  pair.destroy_sessions(&cli_a, &srv_session_a);
  pair.destroy_sessions(&cli_b, &srv_session_b);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
} // TYPED_TEST_P(Session_connect_test, Mq_msg_size_limit_per_session)

/* async_accept() targeting a Server_session that is not in NULL state: per contract the target is made as-if
 * default-cted synchronously, at the start of the call -- the old session's destruction happens right there, in the
 * calling thread -- and the accept then proceeds into the emptied target.  Asserted: right after the call returns, the
 * target is in NULL state (nil token); the old session's opposing client observes the session's end via its error
 * handler; a new client then connects into that same target object, which reaches PEER paired with that client.
 *
 * SHM-jemalloc choreography: the old session's dtor (inside our async_accept() call) blocks until its opposing
 * client's dtor begins -- so a helper thread destroys that client once the client has observed the hosing.  (The
 * other session types do not block; the helper's action is then merely tidy.) */
TYPED_TEST_P(Session_connect_test, Accept_target_reset)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using flow::async::Single_thread_task_loop;
  using boost::chrono::seconds;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("TargetReset");
  pair.remove_server_persistent_bits(false);
  this->start_server(&pair);

  // Session 1: cli_1 <-> srv_session, PEER.  cli_1's error handler fulfills the promise once the session is hosed.
  boost::promise<void> cli_1_hosed_promise;
  const boost::shared_future<void> cli_1_hosed = cli_1_hosed_promise.get_future().share();
  const auto cli_1_ptr
    = boost::make_shared<Client_session>(this->ipc_logger(), pair.m_cli_app, pair.m_srv_app,
                                         [&cli_1_hosed_promise](const Error_code&)
                                           { cli_1_hosed_promise.set_value(); });
  Server_session srv_session;
  this->connect_ok(&pair, cli_1_ptr.get(), &srv_session);
  ASSERT_FALSE(srv_session.session_token().is_nil());

  // The helper: once cli_1 sees session 1 end, destroy cli_1 (see doc comment above regarding SHM-jemalloc).
  Single_thread_task_loop ender_thread{nullptr, "tgt_ender"};
  ender_thread.start();
  ender_thread.post([cli_1_ptr, cli_1_hosed]()
  {
    cli_1_hosed.wait();
    *cli_1_ptr = Client_session{};
  });

  FLOW_LOG_INFO("async_accept() targeting the PEER-state session: it must be emptied synchronously, right now.");
  const auto outcome = this->post_accept(&pair, &srv_session);
  EXPECT_TRUE(srv_session.session_token().is_nil()) << "Target must be as-if default-cted once async_accept() returns.";
  EXPECT_EQ(cli_1_hosed.wait_for(seconds(5)), boost::future_status::ready)
    << "Old session's opposing client did not observe the session's end.";
  ender_thread.stop(); // Joins the helper: cli_1 is destroyed.

  FLOW_LOG_INFO("A new client connects; the accept lands in the emptied target.");
  Client_session cli_2{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Error_code err_code;
  EXPECT_TRUE(cli_2.sync_connect(cli_2.mdt_builder(), nullptr, nullptr, nullptr, &err_code));
  EXPECT_FALSE(err_code) << "[" << err_code << "] [" << err_code.message() << "].";
  Error_code accept_err;
  this->await_accept(outcome, &accept_err);
  EXPECT_FALSE(accept_err);
  srv_session.init_handlers([](const Error_code&) {});
  EXPECT_FALSE(srv_session.session_token().is_nil());
  EXPECT_EQ(srv_session.session_token(), cli_2.session_token());

  pair.destroy_sessions(&cli_2, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
} // TYPED_TEST_P(Session_connect_test, Accept_target_reset)

/* A PEER-state Server_session outliving its Session_server: allowed (see the lifetime tenets in Session_server doc
 * header), and it must keep working.  Beyond merely remaining intact: it actively opens a channel (the MQ-size
 * setting having been captured at accept time -- the regression aspect shared with Mq_msg_size_limit_per_session),
 * with the client passive-accepting; and, for the SHM-backed types, the app-scope arena -- owned jointly by the
 * Session_server and its sessions -- remains usable: an object constructed in it round-trips via lend/borrow. */
TYPED_TEST_P(Session_connect_test, Session_outlives_server)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;
  using boost::chrono::seconds;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("Outlives");
  pair.remove_server_persistent_bits(false);
  this->start_server(&pair);

  struct Passive_open_slot
  {
    boost::promise<void> m_opened;
    Channel_obj m_chan;
  };
  const auto slot = boost::make_shared<Passive_open_slot>();
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {},
                     [slot](Channel_obj&& new_chan, auto&& /*mdt_reader*/)
  {
    slot->m_chan = std::move(new_chan);
    slot->m_opened.set_value();
  }};
  Server_session srv_session;
  this->connect_ok(&pair, &cli, &srv_session);

  FLOW_LOG_INFO("Destroying the Session_server; its session lives on and must keep working.");
  pair.m_srv.reset();

  EXPECT_FALSE(srv_session.session_token().is_nil());
  EXPECT_EQ(srv_session.session_token(), cli.session_token());
  {
    Channel_obj chan;
    Error_code chan_err;
    ASSERT_TRUE(srv_session.open_channel(&chan, &chan_err));
    EXPECT_FALSE(chan_err) << "[" << chan_err << "] [" << chan_err.message() << "].";
    ASSERT_EQ(slot->m_opened.get_future().wait_for(seconds(5)), boost::future_status::ready);
    slot->m_chan = Channel_obj{}; // Channel ends predecease the session.
  }

  if constexpr(Pair::S_SHM_TYPE_OR_NONE != schema::ShmType::NONE)
  {
    auto* const app_arena = srv_session.app_shm();
    ASSERT_NE(app_arena, nullptr);
    { // Scope: handles must be dropped before the sessions are destroyed.
      const auto obj = app_arena->template construct<int>(4242);
      ASSERT_TRUE(obj);
      const auto blob = srv_session.template lend_object<int>(obj);
      EXPECT_FALSE(blob.empty());
      const auto borrowed = cli.template borrow_object<int>(blob);
      ASSERT_TRUE(borrowed);
      EXPECT_EQ(*borrowed, 4242);
    }
  }

  pair.destroy_sessions(&cli, &srv_session);
  pair.remove_server_persistent_bits();
} // TYPED_TEST_P(Session_connect_test, Session_outlives_server)

/* The graceful session-end choreography, server side initiating.  See run_graceful_end() doc for the
 * list of what is covered (error-handler firing semantics both sides; the SHM-jemalloc dtor gate vs.
 * the other types' prompt dtor; hosed-session API sentinels). */
TYPED_TEST_P(Session_connect_test, Graceful_end_srv_initiates)
{
  this->run_graceful_end(true);
}

// As Graceful_end_srv_initiates but the client side initiates; the impl is asymmetric, so run both.
TYPED_TEST_P(Session_connect_test, Graceful_end_cli_initiates)
{
  this->run_graceful_end(false);
}

/* The Session_server run-time config knobs -- exactly 2 exist: mq_msg_size_limit() (all server types;
 * given that our standard config enables MQs) and pool_size_limit_mi() (SHM-classic only).  Each:
 * get/set/read-back; the MQ one rounds a set value up to a max-alignment multiple; and a server with
 * adjusted knobs still vends a working session.  (pool_size_limit_mi()'s actual pool-capping effect is
 * exercised elsewhere: e.g., struc-channel tests use it, via test_util's knob, to make a small
 * characterizable pool.)  Both knobs are set before any accept activity, per their thread-safety docs. */
TYPED_TEST_P(Session_connect_test, Server_knobs)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using flow::util::round_to_multiple;
  using flow::util::max_align_sz;
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("Knobs");
  pair.remove_server_persistent_bits(false);
  this->start_server(&pair, false); // Uncapped: we observe the pool-size default below.
  auto& srv = *pair.m_srv;

  EXPECT_EQ(srv.mq_msg_size_limit(), 0u); // 0 = will-choose-default.
  srv.mq_msg_size_limit(1009); // Deliberately not an alignment multiple.
  EXPECT_EQ(srv.mq_msg_size_limit(), round_to_multiple(size_t(1009), max_align_sz()));

  if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::CLASSIC)
  {
    EXPECT_GT(srv.pool_size_limit_mi(), 0u); // Some sane default.
    srv.pool_size_limit_mi(64);
    EXPECT_EQ(srv.pool_size_limit_mi(), 64u);
  }

  FLOW_LOG_INFO("Knobs set and read back as expected; server must still vend a working session.");
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
  Server_session srv_session;
  this->connect_ok(&pair, &cli, &srv_session);

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
}

/* The SHM accessor shape of each session type, plus basic accessor liveness.  Namely:
 *   - Compile-time: vanilla sessions have no session_shm()/app_shm() at all; SHM-classic has both on
 *     both sides; SHM-jemalloc has session_shm() on both sides but app_shm() on the server side only
 *     (an arena-lending provider's client has no app-scope arena of its own -- deliberate API shape).
 *   - Run-time, in PEER state: the accessors return non-null; Session_server::app_shm(Client_app)
 *     agrees with the per-session accessor; SHM-jemalloc's shm_session() is non-null on both sides.
 *   - SHM-classic: the Session-level lend_object()/borrow_object() wrappers route correctly for
 *     *both* scopes -- a session-scope-arena object and an app-scope-arena object each round-trip
 *     through them with content intact (the arena choice is encoded in the lend blob).
 *     (The SHM-jemalloc Session-level lend/borrow forwarding to shm_session() is exercised
 *     end-to-end -- with content checks -- by transport_test exercise-mode; not repeated here.) */
TYPED_TEST_P(Session_connect_test, Shm_accessors)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Session_server = typename TestFixture::Session_server;

  if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::NONE)
  {
    static_assert((!Has_session_shm<Client_session>::value) && (!Has_session_shm<Server_session>::value)
                    && (!Has_app_shm<Client_session>::value) && (!Has_app_shm<Server_session>::value)
                    && (!Has_session_shm_ptr<Client_session>::value) && (!Has_app_shm_ptr<Server_session>::value)
                    && (!Has_shm_session<Client_session>::value) && (!Has_shm_session<Server_session>::value)
                    && (!Has_shm_reader_config<Client_session>::value)
                    && (!Has_app_shm_builder_config<Server_session>::value)
                    && (!Has_app_shm_lender_session<Server_session>::value)
                    && (!Has_app_shm_reader_config<Client_session>::value),
                  "Vanilla sessions must expose no SHM accessors/configs.");
    static_assert((!Has_server_app_shm<Session_server>::value)
                    && (!Has_server_app_shm_ptr<Session_server>::value)
                    && (!Has_server_app_shm_builder_config<Session_server>::value)
                    && (!Has_server_app_shm_lender_session<Session_server>::value)
                    && (!Has_server_app_shm_reader_config<Session_server>::value),
                  "Vanilla Session_server must expose no app-scope SHM APIs.");
    return; // Nothing further to do at run-time.
  }
  else // if constexpr(SHM-backed either way): the meat.
  {
    if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::CLASSIC)
    {
      static_assert(Has_session_shm<Client_session>::value && Has_session_shm<Server_session>::value
                      && Has_app_shm<Client_session>::value && Has_app_shm<Server_session>::value
                      && Has_app_shm_builder_config<Client_session>::value
                      && Has_app_shm_builder_config<Server_session>::value
                      && Has_app_shm_lender_session<Client_session>::value
                      && Has_app_shm_lender_session<Server_session>::value
                      && Has_app_shm_reader_config<Client_session>::value
                      && Has_app_shm_reader_config<Server_session>::value,
                    "SHM-classic sessions must expose the full session-level SHM API set on both sides.");
      static_assert((!Has_shm_session<Client_session>::value) && (!Has_shm_session<Server_session>::value)
                      && (!Has_shm_reader_config<Client_session>::value)
                      && (!Has_shm_reader_config<Server_session>::value)
                      && (!Has_session_shm_ptr<Client_session>::value)
                      && (!Has_session_shm_ptr<Server_session>::value)
                      && (!Has_app_shm_ptr<Server_session>::value),
                    "SHM-classic: no separate Shm_session concept; nor shared_ptr arena-handle vendors.");
      static_assert(Has_server_app_shm<Session_server>::value
                      && Has_server_app_shm_builder_config<Session_server>::value
                      && Has_server_app_shm_lender_session<Session_server>::value
                      && Has_server_app_shm_reader_config<Session_server>::value
                      && (!Has_server_app_shm_ptr<Session_server>::value),
                    "SHM-classic Session_server: full app-scope API including lender/reader "
                      "(the arena is itself the lend/borrow engine; no per-session state needed).");
    }
    else
    {
      static_assert(Has_session_shm<Client_session>::value && Has_session_shm<Server_session>::value
                      && Has_session_shm_ptr<Client_session>::value
                      && Has_session_shm_ptr<Server_session>::value
                      && Has_shm_session<Client_session>::value && Has_shm_session<Server_session>::value
                      && Has_shm_reader_config<Client_session>::value
                      && Has_shm_reader_config<Server_session>::value
                      && Has_app_shm_reader_config<Client_session>::value
                      && Has_app_shm_reader_config<Server_session>::value,
                    "SHM-jemalloc sessions: session-scope accessors, shm_session()/shm_reader_config(), "
                      "and -- borrowing being scope-agnostic -- app_shm_reader_config(), all on both sides.");
      static_assert(Has_app_shm<Server_session>::value && (!Has_app_shm<Client_session>::value)
                      && Has_app_shm_ptr<Server_session>::value && (!Has_app_shm_ptr<Client_session>::value)
                      && Has_app_shm_builder_config<Server_session>::value
                      && (!Has_app_shm_builder_config<Client_session>::value)
                      && Has_app_shm_lender_session<Server_session>::value
                      && (!Has_app_shm_lender_session<Client_session>::value),
                    "SHM-jemalloc: the app-scope arena belongs to the server side; a client can read "
                      "(borrow) from it but has no arena of its own to expose, allocate in, or lend from.");
      static_assert(Has_server_app_shm<Session_server>::value
                      && Has_server_app_shm_ptr<Session_server>::value
                      && Has_server_app_shm_builder_config<Session_server>::value
                      && (!Has_server_app_shm_lender_session<Session_server>::value)
                      && (!Has_server_app_shm_reader_config<Session_server>::value),
                    "SHM-jemalloc Session_server: arena access + builder config, yes; but lending/reading "
                      "requires a per-session Shm_session, which a session-less server lacks.");
    }

    const auto pair_ptr = boost::make_shared<Pair>();
    auto& pair = *pair_ptr;
    pair.populate_apps("ShmAcc");
    pair.remove_server_persistent_bits(false);
    this->start_server(&pair);
    Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app, [](const Error_code&) {}};
    Server_session srv_session;
    this->connect_ok(&pair, &cli, &srv_session);

    auto* const srv_arena = srv_session.session_shm();
    ASSERT_NE(srv_arena, nullptr);
    ASSERT_NE(cli.session_shm(), nullptr);
    auto* const srv_app_arena = srv_session.app_shm();
    ASSERT_NE(srv_app_arena, nullptr);
    EXPECT_EQ(pair.m_srv->app_shm(pair.m_cli_app), srv_app_arena);

    if constexpr(Pair::S_SHM_TYPE_OR_NONE == schema::ShmType::CLASSIC)
    {
      ASSERT_NE(cli.app_shm(), nullptr); // (Distinct handle object from the server-side ones; same pool.)

      /* The session's SHM-pools are sparse: having opened the session, each has committed (taken RAM for) only a
       * little of its size.  (Guards against regressing to committing entire pools at creation -- as happened with
       * Boost >= 1.76 -- which is costly in RAM and in session-opening time.) */
      for (const auto* const arena : { srv_arena, srv_app_arena })
      {
        EXPECT_LT(ipc::test::shm_pool_committed_sz(arena->m_pool_name), arena->arena_size() / 4)
          << "SHM-pool [" << arena->m_pool_name << "] should be sparse.";
      }

      FLOW_LOG_INFO("SHM-classic: lend/borrow round-trips via the Session-level wrappers, both scopes.");
      { // Scope: borrowed/constructed handles must be dropped before the sessions are destroyed.
        const auto obj_session_scope = srv_arena->template construct<int>(1212);
        const auto blob_1 = srv_session.template lend_object<int>(obj_session_scope);
        EXPECT_FALSE(blob_1.empty());
        const auto borrowed_1 = cli.template borrow_object<int>(blob_1);
        ASSERT_TRUE(borrowed_1);
        EXPECT_EQ(*borrowed_1, 1212);

        const auto obj_app_scope = srv_app_arena->template construct<int>(6767);
        const auto blob_2 = srv_session.template lend_object<int>(obj_app_scope);
        EXPECT_FALSE(blob_2.empty());
        const auto borrowed_2 = cli.template borrow_object<int>(blob_2);
        ASSERT_TRUE(borrowed_2);
        EXPECT_EQ(*borrowed_2, 6767);
      }
    }
    else // if constexpr(JEMALLOC)
    {
      EXPECT_NE(srv_session.shm_session(), nullptr);
      EXPECT_NE(cli.shm_session(), nullptr);

      // The shared_ptr-vending accessor variants agree with their raw-pointer siblings.
      EXPECT_EQ(srv_session.session_shm_ptr().get(), srv_arena);
      EXPECT_EQ(cli.session_shm_ptr().get(), cli.session_shm());
      EXPECT_EQ(srv_session.app_shm_ptr().get(), srv_app_arena);
      EXPECT_EQ(pair.m_srv->app_shm_ptr(pair.m_cli_app).get(), srv_app_arena);

      FLOW_LOG_INFO("SHM-jemalloc: basic per-side-arena liveness (construct/free in each side's own).");
      { // Scope: ditto above.
        const auto obj_srv_side = srv_arena->template construct<int>(1212);
        ASSERT_TRUE(obj_srv_side);
        EXPECT_EQ(*obj_srv_side, 1212);
        const auto obj_cli_side = cli.session_shm()->template construct<int>(6767);
        ASSERT_TRUE(obj_cli_side);
        EXPECT_EQ(*obj_cli_side, 6767);
      }
    }

    pair.destroy_sessions(&cli, &srv_session);
    pair.m_srv.reset();
    pair.remove_server_persistent_bits();
  } // else if constexpr(SHM-backed)
} // TYPED_TEST_P(Session_connect_test, Shm_accessors)

REGISTER_TYPED_TEST_SUITE_P(Session_connect_test,
                            No_server, Corrupt_cns, Stale_cns_then_retry, Rejected_identities, Server_self_check,
                            Config_mismatch, Almost_peer, Null_state, Passive_open_rejected, Open_channel_crossing,
                            Open_channel_peer_closes_immediately, Open_channel_peer_closes_immediately_srv_opens,
                            Accept_abort, Server_destroyed_mid_flight, Session_end_during_connect,
                            Concurrent_accepts, Sync_io_adapter_guards,
                            Mq_msg_size_limit_per_session, Accept_target_reset, Session_outlives_server,
                            Graceful_end_srv_initiates, Graceful_end_cli_initiates, Server_knobs, Shm_accessors);

} // Anonymous namespace

} // namespace ipc::session::test
