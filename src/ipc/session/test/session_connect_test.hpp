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
 *   - Connect attempted when no server has ever run: there is no CNS (PID) file to read.  The client must
 *     fail gracefully with the OS's file-not-found error; and the same Client_session object must then be
 *     usable to connect successfully, once a server does run.
 *   - Connect against a corrupt CNS (PID) file: each of the two malformed-contents paths (no
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
 *   - Compile-time session-config mismatch between the two sides (e.g., differing MQ-type template
 *     parameter): rejected similarly to the above.
 *   - The almost-PEER state of a freshly-accepted Server_session (init_handlers() not yet called):
 *     the documented API no-ops/sentinels are in force; everything comes alive after init_handlers().
 *   - Channel passive-open rejection: a peer constructed without a passive-open handler causes the
 *     opposing side's active open_channel() to emit the specific non-fatal error; the session survives.
 *   - A pending async_accept() aborted by Session_server destruction: its handler must fire, with the
 *     specific object-shutdown code.
 *   - Two async_accept()s outstanding concurrently, satisfied by two clients: both complete; the two
 *     resulting sessions coexist and are correctly paired.
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
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/session/error.hpp"
#include "ipc/shm/arena_lend/arena_lend_fwd.hpp"
#include "ipc/test/test_logger.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <type_traits>

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

  // Constructs (into `*pair.m_srv`) a Session_server for the pair's App universe; it listens immediately.
  void start_server(Pair* pair)
  {
    pair->m_srv = std::make_unique<Session_server>(ipc_logger(),
                                                   pair->m_srv_apps.find(pair->m_srv_app.m_name)->second,
                                                   pair->m_cli_apps);
  }

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

protected:
  // For the test's own narration (INFO); also the optional ipc_logger() target.
  Test_logger m_test_logger{Sev::S_INFO};
}; // class Session_connect_test

TYPED_TEST_SUITE_P(Session_connect_test);

/* No server has ever run: no CNS (PID) file exists.  Connect must fail with the OS file-not-found error;
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

  FLOW_LOG_INFO("Connect attempt with no server ever having run (hence no CNS (PID) file).");
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

/* The CNS (PID) file exists but is corrupt.  Each of the two malformed-contents code paths must yield
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

  FLOW_LOG_INFO("Connect attempts against corrupt CNS (PID) file [" << cns_path << "]: 2 corruption sorts.");
  connect_expecting_bad_format("notanumber\n"); // Proper line; does not parse as a PID.
  connect_expecting_bad_format("12345"); // Would parse fine; but no newline-terminated line = not proper.

  /* Clean slate: the CNS file (hand-made above) and its mutex (created by the client during the attempts)
   * both exist; standard removal applies. */
  pair.remove_server_persistent_bits();
}

/* The stale-CNS scenario -- in production terms: server instance 1 ran and is gone (cleanly or not: for
 * a client the observable is the same); the CNS (PID) file remains -- nothing ever deletes it, on purpose;
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
  this->start_server(&pair);
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
                            No_server, Corrupt_cns, Stale_cns_then_retry, Rejected_identities,
                            Config_mismatch, Almost_peer, Passive_open_rejected, Accept_abort,
                            Concurrent_accepts, Graceful_end_srv_initiates, Graceful_end_cli_initiates,
                            Server_knobs, Shm_accessors);

} // Anonymous namespace

} // namespace ipc::session::test
