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

namespace ipc::session::test
{

namespace
{

// A column of the test matrix; see similarly named .hpp.
using Jemalloc_pair_types = ::testing::Types<Cfg_session_pair<schema::ShmType::JEMALLOC>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Shm_type, Session_connect_test, Jemalloc_pair_types, Pair_type_names);

/* @todo Test SHM-jemalloc arena-creation failure during session opening: it must fail just that session (the
 * accept and/or connect emits an Error_code), not exit() the process (Arena::create() throws; the ipc::session
 * SHM-jemalloc layer catches it).  Capping RLIMIT_NOFILE cannot target the arena creation specifically (session
 * opening needs descriptors earlier).  A deterministic trigger: jemalloc's opt.retain disabled, which makes every
 * jemalloc::Memory_manager::create_arena() throw (errc::not_supported); but it is read-only at process start.
 * Possible approach: a gtest death-test, `threadsafe` style (re-executes the binary), with the parent setting
 * `{$prefix}MALLOC_CONF=retain:false`; the child runs a SHM-jemalloc Session_server + Client_session pair,
 * expects the accept/connect to fail with not_supported, then `std::exit(0)`; the parent EXPECT_EXIT()s with
 * code 0.  (This also covers Memory_manager::create_arena()'s opt.retain check itself which isn't currently tested.) */

} // Anonymous namespace

} // namespace ipc::session::test
