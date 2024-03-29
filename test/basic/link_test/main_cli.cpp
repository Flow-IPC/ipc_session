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
#include <ipc/session/client_session.hpp>

/* This little thing is *not* a unit-test; it is built to ensure the proper stuff links through our
 * build process.  We try to use a compiled thing or two; and a template (header-only) thing or two;
 * not so much for correctness testing but to see it build successfully and run without barfing. */
int main(int argc, char const * const * argv)
{
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::Flow_log_component;
  using std::exception;
  using std::optional;

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  optional<Simple_ostream_logger> std_logger;
  optional<Async_file_logger> log_logger;
  setup_logging(&std_logger, &log_logger, argc, argv, false);
  FLOW_LOG_SET_CONTEXT(&(*std_logger), Flow_log_component::S_UNCAT);

  try
  {
    ensure_run_env(argv[0], false);

    // Please see main_srv.cpp.  We're just the other side of that.  Keeping comments light.

    ipc::session::Client_session<ipc::session::schema::MqType::NONE, false>
      session(&(*log_logger),
              CLI_APPS.find(CLI_NAME)->second,
              SRV_APPS.find(SRV_NAME)->second, [](auto&&...) {});

    FLOW_LOG_INFO("Session-client attempting to open session against session-server; "
                  "it'll either succeed or fail very soon; and at that point we will exit.");

    session.sync_connect(); // Let it throw on error.

    FLOW_LOG_INFO("Session opened: [" << session << "].");
    FLOW_LOG_INFO("Exiting.");
  } // try
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    FLOW_LOG_WARNING("(Perhaps you did not execute session-server executable in parallel, or "
                     "you executed one or both of us oddly?)");
    return 1;
  }

  return 0;
} // main()
