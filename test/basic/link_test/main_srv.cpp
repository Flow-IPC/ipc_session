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

#include "common.hpp"
#include <ipc/session/session_server.hpp>
#include <flow/log/simple_ostream_logger.hpp>
#include <flow/log/async_file_logger.hpp>

/* This little thing is *not* a unit-test; it is built to ensure the proper stuff links through our
 * build process.  We try to use a compiled thing or two; and a template (header-only) thing or two;
 * not so much for correctness testing but to see it build successfully and run without barfing. */
int main(int argc, char const * const * argv)
{
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::log::Config;
  using flow::log::Sev;
  using flow::Error_code;
  using flow::Flow_log_component;
  using flow::util::String_view;
  using boost::promise;
  using std::exception;

  constexpr String_view LOG_FILE = "ipc_session_link_test_srv.log";
  constexpr int BAD_EXIT = 1;

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  Config std_log_config;
  std_log_config.init_component_to_union_idx_mapping<Flow_log_component>
    (1000, Config::standard_component_payload_enum_sparse_length<Flow_log_component>());
  std_log_config.init_component_to_union_idx_mapping<ipc::Log_component>
    (2000, Config::standard_component_payload_enum_sparse_length<ipc::Log_component>());
  std_log_config.init_component_names<Flow_log_component>(flow::S_FLOW_LOG_COMPONENT_NAME_MAP, false, "flow-");
  std_log_config.init_component_names<ipc::Log_component>(ipc::S_IPC_LOG_COMPONENT_NAME_MAP, false, "ipc-");
  Simple_ostream_logger std_logger(&std_log_config);
  FLOW_LOG_SET_CONTEXT(&std_logger, Flow_log_component::S_UNCAT);
  // This is separate: the IPC/Flow logging will go into this file.
  const auto log_file = (argc >= 2) ? String_view(argv[1]) : LOG_FILE;
  FLOW_LOG_INFO("Opening log file [" << log_file << "] for IPC/Flow logs only.");
  Config log_config = std_log_config;
  log_config.configure_default_verbosity(Sev::S_DATA, true); // High-verbosity.  Use S_INFO in production.
  /* First arg: could use &std_logger to log-about-logging to console; but it's a bit heavy for such a console-dependent
   * little program.  Just just send it to /dev/null metaphorically speaking. */
  Async_file_logger log_logger(nullptr, &log_config, log_file, false /* No rotation; we're no serious business. */);

  try
  {
    /* Use Server_session template and some other peripheral things.
     * As a reminder we're not trying to demo the library here; just to access certain things -- probably
     * most users would do something more impressive than this.  We're ensuring stuff built OK
     * more or less.  That said the test here is arguably somewhat more sophisticated than similar link_test
     * programs for dependencies ipc_core and ipc_transport_structured; there are 2 programs involved (this guy
     * and main_cli.cpp counterpart) which interact; and the way it is set up is vaguely realistic-ish.
     * Internally, too, quite a lot of stuff is being exercised; in particular there's an internally
     * used struc::Channel used for establishing the session; so features of ipc_transport_structured are being
     * exercised among other things. */


    ensure_run_env(argv[0], true);

    /* common.[hc]pp has the server/client descriptions which are (as they must be) equal between the client app
     * and this server app. */

    ipc::session::Session_server<ipc::session::schema::MqType::NONE, false>
      srv(&log_logger, SRV_APPS.find(SRV_NAME)->second, CLI_APPS);

    FLOW_LOG_INFO("Session-server started; invoke session-client executable from same CWD; it will open session; "
                  "at that point we will be satisfied and will exit.");

    decltype(srv)::Server_session_obj session;
    promise<void> accepted_promise;
    bool ok = false;
    srv.async_accept(&session, [&](const Error_code& err_code)
    {
      if (err_code)
      {
        FLOW_LOG_WARNING("Error is totally unexpected.  Error: [" << err_code << "] [" << err_code.message() << "].");
      }
      else
      {
        FLOW_LOG_INFO("Session accepted: [" << session << "].");
        ok = true;
      }
      // Either way though:
      accepted_promise.set_value();
    });

    accepted_promise.get_future().wait();
    if (ok)
    {
      session.init_handlers([](const Error_code&) {});

      // Don't judge us.  Again, we aren't demo-ing best practices here!
      FLOW_LOG_INFO("Sleeping for a few sec to avoid yanking session away from other side right after opening it.  "
                    "This is not intended to demonstrate a best practice -- just acting a certain way in a "
                    "somewhat contrived short-lived-session scenario; essentially so that on the client side it "
                    "can \"savor\" the newly-open session, before we take it down right away.");
      flow::util::this_thread::sleep_for(boost::chrono::seconds(1));
    }

    FLOW_LOG_INFO("Exiting.");
  } // try
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return BAD_EXIT;
  }

  return 0;
} // main()
