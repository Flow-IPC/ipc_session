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
using Classic_pair_types = ::testing::Types<Cfg_session_pair<schema::ShmType::CLASSIC>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Shm_type, Session_connect_test, Classic_pair_types, Pair_type_names);

} // Anonymous namespace

} // namespace ipc::session::test
