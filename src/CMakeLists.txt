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

# See FlowLikeLib.cmake per below; it'll explain inside.
# It mandates the following procedure and documents details.

# We just need executables to link ipc_transport_structured (and whatever it says it must link).
# Other stuff is transitively specified by the latter, even though we do use some of that
# stuff directly too.

message(STATUS "Dependents shall need to import/link libs: ipc_transport_structured.")

set(DEP_IPC_TRANSPORT_STRUCTURED_VERSION 1.0)
if(TARGET ipc_transport_structured)
  set(DEP_LIBS ipc_transport_structured) # In meta-project mode it's loaded by this short name.

  # In meta-project mode we lack the benefit of find_package(Ipc*) (see the other if() path); which means
  # the transitive dependency -- capnp -- has not been communicated to us.  So we must do it ourselves to
  # be able to do capnp stuff ourselves.
  find_package(CapnProto ${CAPNP_VER} CONFIG REQUIRED)
else()
  find_package(IpcTransportStructured ${DEP_IPC_TRANSPORT_STRUCTURED_VERSION} CONFIG REQUIRED)
  # In separate-project mode it's been install(EXPORT)ed with the namespace prefix.
  set(DEP_LIBS IpcTransportStructured::ipc_transport_structured)
endif()

# Mirror above find_package() calls.  As directed omit "REQUIRED."
set(DEP_LIBS_PKG_ARG_LISTS "IpcTransportStructured ${DEP_IPC_TRANSPORT_STRUCTURED_VERSION} CONFIG")

# (See above if wondering why we're not auto-searching for these.  There's a good reason.)
# Ideally keep this in `find -s` order.
set(SRCS
    ipc/session/app.cpp
    ipc/session/detail/session_shared_name.cpp
    ipc/session/error.cpp)
set(CAPNP_SCHEMAS
    ipc/session/schema/common.capnp
    ipc/session/schema/detail/session_master_channel.capnp)
set(HDRS
    ipc/session/app.hpp
    ipc/session/client_session.hpp
    ipc/session/detail/client_session_impl.hpp
    ipc/session/detail/server_session_dtl.hpp
    ipc/session/detail/server_session_impl.hpp
    ipc/session/detail/session_base.hpp
    ipc/session/detail/session_fwd.hpp
    ipc/session/detail/session_server_impl.hpp
    ipc/session/detail/session_shared_name.hpp
    ipc/session/error.hpp
    ipc/session/server_session.hpp
    ipc/session/session.hpp
    ipc/session/session_fwd.hpp
    ipc/session/session_server.hpp
    ipc/session/sync_io/client_session_adapter.hpp
    ipc/session/sync_io/detail/session_adapter.hpp
    ipc/session/sync_io/server_session_adapter.hpp
    ipc/session/sync_io/session_server_adapter.hpp
    ${CAPNP_SCHEMAS}) # Export these like any regular headers...

# ...but also, at the proper time, generate .c++ from them (add to SRCS) and .h also from them (add to HDRS).
# Reminder: this is a supported optional hook of FlowLikeLib.cmake.
function(generate_custom_srcs)
  if(TARGET ipc_transport_structured)
    # In meta-project mode our dependency's (ipc_transport_structured's) schemas have not (necessarily) been
    # exported yet: we are within `make` (or equivalent), not past any `make install`, so
    # any `import` in a .capnp of ours (in CAPNP_SCHEMAS) that tries to access such a dependency .capnp
    # will fail.  I fail to see a significantly better approach than to provide the path manually based
    # on where the dependency's source code should sit in a sibling subdir off FLOW_LIKE_META_ROOT (or off
    # ${CMAKE_CURRENT_SOURCE_DIR}/..; same thing).
    #
    # Note that the analogous C++ #include will work fine, because CMake would have propagated the include
    # root automatically due to the BUILD_INTERFACE: specification.  However such a thing is not available --
    # that I (ygoldfel) know of -- in the case of capnp's import mechanism.
    #
    # Lastly note that we'll cumulatively need to do this for all of our dependents also needing similar
    # `.capnp`s to be importable.
    set(CAPNPC_IMPORT_DIRS ${FLOW_LIKE_META_ROOT_ipc_transport_structured}/src)

    message(VERBOSE "We are a subdir of a meta-project.  Manually added the following sibling paths to the capnp "
                      "import paths: [${CAPNPC_IMPORT_DIRS}].")
  endif()

  capnp_generate_cpp(capnp_generated_srcs capnp_generated_hdrs ${CAPNP_SCHEMAS})

  list(APPEND SRCS ${capnp_generated_srcs})
  list(APPEND HDRS ${capnp_generated_hdrs})
  set(SRCS ${SRCS} PARENT_SCOPE)
  set(HDRS ${HDRS} PARENT_SCOPE)

  message(STATUS "Install target: Exports capnp schemas [${CAPNP_SCHEMAS}].")
  message(STATUS "Install target: Exports capnp schema-generated headers [${capnp_generated_hdrs}].")
  message(STATUS "capnp-generated sources: [${capnp_generated_srcs}].")
endfunction()

include("${FLOW_LIKE_TOOLS}/FlowLikeLib.cmake")
