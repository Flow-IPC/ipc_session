# Flow-IPC: Sessions
# Copyright 2023 Akamai Technologies, Inc.
#
# Licensed under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in
# compliance with the License.  You may obtain a copy
# of the License at
#
#   https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in
# writing, software distributed under the License is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing
# permissions and limitations under the License.

# Make a couple binaries in very similar ways; call this for each.
function(handle_binary name_sh) # name_sh is "cli" or "srv"; as in session-client or session-server.
  set(name "ipc_session_link_test_${name_sh}.exec") # Must match common.cpp constant values.
  add_executable(${name} common.cpp "main_${name_sh}.cpp")

  # Do stuff we've resolved to do on all our targets.
  common_set_target_properties(${name})

  # Link good ol' libipc_session.
  target_link_libraries(${name} PRIVATE ${PROJ})

  # Might as well export if they `make install` or equivalent.
  install(TARGETS ${name}
          RUNTIME DESTINATION bin)

  message(STATUS "Recommended: [cd ${CMAKE_INSTALL_PREFIX}/bin && "
                   "./${name}].")
  message(STATUS "Run srv program first in 1 terminal, then cli in another, as same user, from that dir.")
endfunction()

handle_binary(srv)
handle_binary(cli)
