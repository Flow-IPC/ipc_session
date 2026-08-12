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

#include "ipc/session/test/session_connect_test.hpp"
#include "ipc/util/detail/util_fwd.hpp"
#include <boost/asio/connect_pipe.hpp>
#include <cstring>
#include <vector>

namespace ipc::session::test
{

namespace
{

// A column of the test matrix; see similarly named .hpp.
using Vanilla_pair_types = ::testing::Types<Cfg_session_pair<schema::ShmType::NONE>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Shm_type, Session_connect_test, Vanilla_pair_types, Pair_type_names);

/* ---------- The channel-establishment config-matrix suite. ----------
 *
 * Per the master testing plan, the tests of the similarly named .hpp run in one channel/transport config;
 * the suite below covers the config axis itself: each of the 6 configs (MQ type NONE/BIPC/POSIX x
 * native-handles transport on/off), on vanilla sessions (channel establishment is vanilla-layer machinery;
 * the SHM-aware session types add nothing to it).  Per config: a session opens (thereby also covering
 * session-open-per-config); channels get established each of the ways that exist -- init-channel:
 * requested by either side; active open_channel(): from either side, passive-accepted by the other --
 * and each resulting channel proves basically operational, per constituent pipe: a blob crosses the
 * blobs pipe (if the config has one); a native handle crosses the handles pipe (if any) and is proven
 * *alive* -- not merely non-null -- by writing a byte through the received descriptor.  (For the
 * MQ-less native-handles config the one pipe is a handles pipe: the handle round, with its meta-blob
 * content check, is its works-check.)  No struc::Channel upgrade here: structured transport atop
 * established channels is covered elsewhere. */

template<schema::MqType S_MQ_TYPE, bool S_HANDLES>
using Cfg_matrix_pair = Session_pair<S_MQ_TYPE, S_HANDLES, schema::ShmType::NONE>;

using Matrix_pair_types = ::testing::Types<Cfg_matrix_pair<schema::MqType::NONE, true>,
                                           Cfg_matrix_pair<schema::MqType::NONE, false>,
                                           Cfg_matrix_pair<schema::MqType::BIPC, true>,
                                           Cfg_matrix_pair<schema::MqType::BIPC, false>,
                                           Cfg_matrix_pair<schema::MqType::POSIX, true>,
                                           Cfg_matrix_pair<schema::MqType::POSIX, false>>;

// Readable per-instantiation names: mqPosixHndl, mqNoneNoHndl, etc.
struct Matrix_type_names
{
  template<typename Session_pair_t>
  static string GetName(int) // (Name style is an exception: mandated by gtest.)
  {
    string name;
    switch (Session_pair_t::S_MQ_TYPE_OR_NONE)
    {
      case schema::MqType::NONE: name = "mqNone"; break;
      case schema::MqType::BIPC: name = "mqBipc"; break;
      case schema::MqType::POSIX: name = "mqPosix"; break;
      default: assert(false);
    }
    return name + (Session_pair_t::S_TRANSMIT_NATIVE_HANDLES ? "Hndl" : "NoHndl");
  }
};

// The matrix suite's fixture: the base scaffolding plus the per-channel pipe checks.
template<typename Session_pair_t>
class Session_channel_matrix_test : public Session_connect_test<Session_pair_t>
{
public:
  using Pair = Session_pair_t;
  using Channel_obj = typename Pair::Client_session_t::Channel_obj;

  /* Sends one blob (if the config has a blobs pipe) and one native handle (if a handles pipe) from
   * `*chan_snd` to `*chan_rcv` -- async-I/O-pattern channels -- verifying content.  The handle is
   * proven alive by the receiver writing a byte through it, observed on the sender's retained OS-pipe
   * end.  Direction matters (e.g., with MQs each direction is its own MQ), so callers invoke this
   * twice, swapped. */
  template<typename Aio_channel>
  void check_pipes_one_way(Aio_channel* chan_snd, Aio_channel* chan_rcv, const string& ctx)
  {
    using util::Blob_const;
    using util::Blob_mutable;
    using util::Native_handle;
    using boost::chrono::seconds;

    if constexpr(Aio_channel::S_HAS_BLOB_PIPE)
    {
      const string msg = ctx + ":blob";
      /* Sized to the pipe's max: the MQ-backed receivers have S_BLOB_UNDERFLOW_ALLOWED = false,
       * meaning the target buffer must accommodate the largest possible in-message. */
      std::vector<uint8_t> rcv_buf(chan_rcv->receive_blob_max_size());
      Error_code rcv_err;
      size_t rcv_sz = 0;
      boost::promise<void> done;
      EXPECT_TRUE(chan_rcv->async_receive_blob(Blob_mutable{rcv_buf.data(), rcv_buf.size()},
                                               [&](const Error_code& err_code, size_t sz)
      {
        rcv_err = err_code;
        rcv_sz = sz;
        done.set_value();
      }));
      EXPECT_TRUE(chan_snd->send_blob(Blob_const{msg.data(), msg.size()}));
      ASSERT_EQ(done.get_future().wait_for(seconds(5)), boost::future_status::ready)
        << "Blob did not arrive in time; context: [" << ctx << "].";
      EXPECT_FALSE(rcv_err) << "[" << rcv_err << "] [" << rcv_err.message() << "]; context: [" << ctx << "].";
      ASSERT_EQ(rcv_sz, msg.size());
      EXPECT_EQ(std::memcmp(rcv_buf.data(), msg.data(), msg.size()), 0);
    }

    if constexpr(Aio_channel::S_HAS_NATIVE_HANDLE_PIPE)
    {
      using util::Pipe_reader;
      using util::Pipe_writer;
      using boost::asio::connect_pipe;

      /* An anonymous OS pipe supplies the guinea-pig FD (its write end); we retain the read end.
       * Via the ipc::util wrappers -- more portable than raw ::pipe() et al., and they get some free
       * exercising this way. */
      flow::util::Task_engine task_engine;
      Pipe_reader pipe_rd{task_engine};
      Pipe_writer pipe_wr{task_engine};
      Error_code pipe_err;
      connect_pipe(pipe_rd, pipe_wr, pipe_err);
      ASSERT_FALSE(pipe_err);

      const string meta = ctx + ":hndl";
      // Sized to the pipe's max (same rationale as the blobs-pipe buffer above).
      std::vector<uint8_t> rcv_meta_buf(chan_rcv->receive_meta_blob_max_size());
      Native_handle rcv_hndl;
      Error_code rcv_err;
      size_t rcv_sz = 0;
      boost::promise<void> done;
      EXPECT_TRUE(chan_rcv->async_receive_native_handle(&rcv_hndl,
                                                        Blob_mutable{rcv_meta_buf.data(),
                                                                     rcv_meta_buf.size()},
                                                        [&](const Error_code& err_code, size_t sz)
      {
        rcv_err = err_code;
        rcv_sz = sz;
        done.set_value();
      }));
      EXPECT_TRUE(chan_snd->send_native_handle(Native_handle{pipe_wr.native_handle()},
                                               Blob_const{meta.data(), meta.size()}));
      ASSERT_EQ(done.get_future().wait_for(seconds(5)), boost::future_status::ready)
        << "Native handle did not arrive in time; context: [" << ctx << "].";
      EXPECT_FALSE(rcv_err) << "[" << rcv_err << "] [" << rcv_err.message() << "]; context: [" << ctx << "].";
      ASSERT_EQ(rcv_sz, meta.size());
      EXPECT_EQ(std::memcmp(rcv_meta_buf.data(), meta.data(), meta.size()), 0);
      ASSERT_FALSE(rcv_hndl.null());

      /* The transit proof: the received descriptor must *work*, not merely exist: adopt it into a
       * Pipe_writer; a byte produced through it must emerge on our retained read end (pipe_consume()
       * blocks until it does; on any error therein: undefined behavior/assert per its contract --
       * good enough for a sanity check).  The 3 Pipe_* dtors then close all the FDs, the received
       * one included. */
      Pipe_writer received_wr{task_engine, rcv_hndl.m_native_handle};
      util::pipe_produce(this->get_logger(), &received_wr);
      util::pipe_consume(this->get_logger(), &pipe_rd);
    }
  } // check_pipes_one_way()
}; // class Session_channel_matrix_test

TYPED_TEST_SUITE(Session_channel_matrix_test, Matrix_pair_types, Matrix_type_names);

/* For this instantiation's config: session up; 4 channels established (init-channel x the 2 request
 * flavors; active open_channel() x the 2 directions, passive-accepted by the opposing side -- the
 * passive-open-capable handler wiring being a first for this file); each channel's pipe(s) basically
 * work, in both directions.  See the section comment above. */
TYPED_TEST(Session_channel_matrix_test, Establish_and_basic_use)
{
  FLOW_LOG_SET_CONTEXT(this->get_logger(), Log_component::S_TEST);
  using Pair = typename TestFixture::Pair;
  using Client_session = typename TestFixture::Client_session;
  using Server_session = typename TestFixture::Server_session;
  using Channel_obj = typename TestFixture::Channel_obj;
  using Channels = typename Pair::Channels;
  using boost::chrono::seconds;

  const auto pair_ptr = boost::make_shared<Pair>();
  auto& pair = *pair_ptr;
  pair.populate_apps("ChanMx");
  pair.remove_server_persistent_bits(false);
  this->start_server(&pair);

  // Accept, with the channel-bearing arg set (contrast the base's post_accept()): 1 init channel by
  // server request; a container for the client-requested one(s).
  Server_session srv_session;
  Channels srv_init_chans_by_srv_req;
  Channels srv_init_chans_by_cli_req;
  typename TestFixture::Accept_outcome accept_outcome;
  pair.m_srv->async_accept(&srv_session,
                           &srv_init_chans_by_srv_req,
                           nullptr, // mdt_from_cli_or_null
                           &srv_init_chans_by_cli_req,
                           [](auto&&...) { return 1; }, // n_init_channels_by_srv_req_func
                           [](auto&&...) {}, // mdt_load_func
                           [accept_outcome](const Error_code& err_code)
  {
    *accept_outcome.m_err = err_code;
    accept_outcome.m_done->set_value();
  });

  // Client: passive-open-capable ctor form; its passive-open handler loads this slot.
  struct Passive_open_slot
  {
    boost::promise<void> m_opened;
    Channel_obj m_chan;
  };
  const auto cli_passive_slot = boost::make_shared<Passive_open_slot>();
  Client_session cli{this->ipc_logger(), pair.m_cli_app, pair.m_srv_app,
                     [](const Error_code&) {},
                     [cli_passive_slot](Channel_obj&& new_chan, auto&& /*mdt_reader*/)
  {
    cli_passive_slot->m_chan = std::move(new_chan);
    cli_passive_slot->m_opened.set_value();
  }};

  Channels cli_init_chans_by_cli_req;
  cli_init_chans_by_cli_req.resize(1); // Request 1 init channel from our side.
  Channels cli_init_chans_by_srv_req;
  Error_code err_code;
  EXPECT_TRUE(cli.sync_connect(cli.mdt_builder(), &cli_init_chans_by_cli_req, nullptr,
                               &cli_init_chans_by_srv_req, &err_code));
  EXPECT_FALSE(err_code) << "sync_connect error: [" << err_code << "] [" << err_code.message() << "].";
  Error_code accept_err;
  this->await_accept(accept_outcome, &accept_err);
  EXPECT_FALSE(accept_err);

  const auto srv_passive_slot = boost::make_shared<Passive_open_slot>();
  srv_session.init_handlers([](const Error_code&) {},
                            [srv_passive_slot](Channel_obj&& new_chan, auto&& /*mdt_reader*/)
  {
    srv_passive_slot->m_chan = std::move(new_chan);
    srv_passive_slot->m_opened.set_value();
  });

  // The init channels, both flavors, present on both sides.
  ASSERT_EQ(cli_init_chans_by_cli_req.size(), 1u);
  ASSERT_EQ(srv_init_chans_by_cli_req.size(), 1u);
  ASSERT_EQ(cli_init_chans_by_srv_req.size(), 1u);
  ASSERT_EQ(srv_init_chans_by_srv_req.size(), 1u);

  { // Scope: the active channels + async-I/O upgrades die before the sessions (tidiness, mostly).
    Channel_obj active_cli_side;
    Error_code chan_err;
    ASSERT_TRUE(cli.open_channel(&active_cli_side, &chan_err));
    EXPECT_FALSE(chan_err) << "[" << chan_err << "] [" << chan_err.message() << "].";
    ASSERT_EQ(srv_passive_slot->m_opened.get_future().wait_for(seconds(5)),
              boost::future_status::ready);

    Channel_obj active_srv_side;
    ASSERT_TRUE(srv_session.open_channel(&active_srv_side, &chan_err));
    EXPECT_FALSE(chan_err) << "[" << chan_err << "] [" << chan_err.message() << "].";
    ASSERT_EQ(cli_passive_slot->m_opened.get_future().wait_for(seconds(5)),
              boost::future_status::ready);

    FLOW_LOG_INFO("Session + 4 channels up (init-by-cli-req, init-by-srv-req, active-from-cli, "
                  "active-from-srv); basic per-pipe checks on each, both directions.");

    const auto check_both_ways = [&](Channel_obj& cli_end, Channel_obj& srv_end, const string& ctx)
    {
      auto aio_cli = cli_end.async_io_obj();
      auto aio_srv = srv_end.async_io_obj();
      this->check_pipes_one_way(&aio_cli, &aio_srv, ctx + "/c2s");
      this->check_pipes_one_way(&aio_srv, &aio_cli, ctx + "/s2c");
    };
    check_both_ways(cli_init_chans_by_cli_req.front(), srv_init_chans_by_cli_req.front(), "initCliReq");
    check_both_ways(cli_init_chans_by_srv_req.front(), srv_init_chans_by_srv_req.front(), "initSrvReq");
    check_both_ways(active_cli_side, srv_passive_slot->m_chan, "activeFromCli");
    check_both_ways(cli_passive_slot->m_chan, active_srv_side, "activeFromSrv");
  }

  // Channels (all remaining ends) predecease the sessions, as above.
  cli_init_chans_by_cli_req.clear();
  cli_init_chans_by_srv_req.clear();
  srv_init_chans_by_cli_req.clear();
  srv_init_chans_by_srv_req.clear();
  cli_passive_slot->m_chan = Channel_obj{};
  srv_passive_slot->m_chan = Channel_obj{};

  pair.destroy_sessions(&cli, &srv_session);
  pair.m_srv.reset();
  pair.remove_server_persistent_bits();
} // TYPED_TEST(Session_channel_matrix_test, Establish_and_basic_use)

} // Anonymous namespace

} // namespace ipc::session::test
