# Flow-IPC Sub-project -- Sessions -- Painlessly establishing process-to-process conversations

This project is a sub-project of the larger Flow-IPC meta-project.  Please see
a similar `README.md` for Flow-IPC, first.  You can most likely find it either in the parent
directory to this one; or else in a sibling GitHub repository named `ipc.git`.

A more grounded description of the various sub-projects of Flow-IPC, including this one, can be found
in `./src/ipc/common.hpp` off the directory containing the present README.  Look for
`Distributed sub-components (libraries)` in a large C++ comment.

Took a look at those?  Still interested in `ipc_session` as an independent entity?  Then read on.
Before you do though: it is, typically, both easier and more functional to simply treat Flow-IPC as a whole.
To do so it is sufficient to never have to delve into topics discussed in this README.  In particular
the Flow-IPC generated documentation guided Manual + Reference are monolithic and cover all the
sub-projects together, including this one.

Still interested?  Then read on.

`ipc_session` depends on `ipc_transport_structured` (and all its dependencies; i.e. `ipc_core`, `flow`).  It provides
`ipc::session` (excluding `ipc::session::shm`).

It is possible to use the structured layer of `ipc::transport`, namely the big daddy `transport::struc::Channel`,
without any help from ipc::session.  (It's also possible to establish unstructured `transport::Channel` and
the various lower-level IPC pipes it might comprise.)  And, indeed,
once a given `struc::Channel` (or `Channel`) is "up," one can and *should*
simply use it to send/receive stuff.  The problem that `ipc::session` solves is in establishing
the infrastructure that makes it simple to (1) open new `struc::Channel`s or `Channel`s or SHM areas;
and (2) properly deal with process lifecycle events such as the starting and ending (gracefully or otherwise) of
the local and partner process.

Regarding (1), in particular (just to give a taste of what one means):

  - What underlying low-level transports will we even be using?  MQs?  Local (Unix domain) stream sockets?
    Both?
  - For each of those, we must connect or otherwise establish each one.  In the case of MQs, say, there has
    to be an agreed-upon `util::Shared_name` for the MQ in each direction... so what is that name?
    How to prevent collisions in this name?

While `ipc::transport` lets one do whatever one wants, with great flexibility, `ipc::session` establishes conventions
for all these things -- typically hiding/encapsulating them away.

Regarding (2) (again, just giving a taste):

  - To what process are we even talking?  What if we want to talk to 2, or 3, processes?  What if we want to talk
    to 1+ processes of application X and 1+ processes of application Y?  How might we segregate the data
    between these?
  - What happens if 1 of 5 instances of application X, to whom we're speaking, goes down?  How to ensure cleanup
    of any kernel-persistence resources, such as the potentially used POSIX MQs, or even of SHM (`ipc::shm`)?

Again -- `ipc::session` establishes conventions for these lifecycle matters and provides key utilities such as
kernel-persistent resource cleanup.

There can be good reasons to use `ipc::transport` without `ipc_session` (though probably fewer good ones
when using `ipc::transport::Channel` or `ipc::transport::struc::Channel`); but *generally* speaking
using `ipc_session` will save lots and lots of pain.  We would even claim that it's the kind of pain
one takes for granted when doing IPC, classically; so when it goes away, it feels quite nice.

## Installation

An exported `ipc_session` consists of C++ header files installed under "ipc/..." in the
include-root; and a library such as `libipc_session.a`.
Certain items are also exported for people who use CMake to build their own
projects; we make it particularly easy to use `ipc_session` proper in that case
(`find_package(IpcSession)`).  However documentation is generated monolithically for all of Flow-IPC;
not for `ipc_session` separately.

The basic prerequisites for *building* the above:

  - Linux;
  - a C++ compiler with C++ 17 support;
  - Boost headers (plus certain libraries) install;
  - {fmt} install;
  - dependency headers and library (from within this overall project) install(s); in this case those of:
    `flow`, `ipc_core`, `ipc_transport_structured`;
  - CMake;
  - Cap'n Proto (a/k/a capnp).

The basic prerequisites for *using* the above:

  - Linux, C++ compiler, Boost, {fmt}, above-listed dependency lib(s), capnp (but CMake is not required); plus:
  - your source code `#include`ing any exported `ipc/` headers must be itself built in C++ 17 mode;
  - any executable using the `ipc_*` libraries must be linked with certain Boost and ubiquitous
    system libraries.

We intentionally omit version numbers and even specific compiler types in the above description; the CMake run
should help you with that.

To build `ipc_session`:

  1. Ensure a Boost install is available.  If you don't have one, please install the latest version at
     [boost.org](https://boost.org).  If you do have one, try using that one (our build will complain if insufficient).
     (From this point on, that's the recommended tactic to use when deciding on the version number for any given
     prerequisite.  E.g., same deal with CMake in step 2.)
  2. Ensure a {fmt} install is available (available at [{fmt} web site](https://fmt.dev/) if needed).
  3. Ensure a CMake install is available (available at [CMake web site](https://cmake.org/download/) if needed).
  4. Ensure a capnp install is available (available at [Cap'n Proto web site](https://capnproto.org/) if needed).
  5. Use CMake `cmake` (command-line tool) or `ccmake` (interactive text-UI tool) to configure and generate
     a build system (namely a GNU-make `Makefile` and friends).  Details on using CMake are outside our scope here;
     but the basics are as follows.  CMake is very flexible and powerful; we've tried not to mess with that principle
     in our build script(s).
     1. Choose a tool.  `ccmake` will allow you to interactively configure aspects of the build system, including
        showing docs for various knobs our CMakeLists.txt (and friends) have made availale.  `cmake` will do so without
        asking questions; you'll need to provide all required inputs on the command line.  Let's assume `cmake` below,
        but you can use whichever makes sense for you.
     2. Choose a working *build directory*, somewhere outside the present `ipc` distribution.  Let's call this
        `$BUILD`: please `mkdir -p $BUILD`.  Below we'll refer to the directory containing the present `README.md` file
        as `$SRC`.
     3. Configure/generate the build system.  The basic command line:
        `cd $BUILD && cmake -DCMAKE_INSTALL_PREFIX=... -DCMAKE_BUILD_TYPE=... $SRC`,
        where `$CMAKE_INSTALL_PREFIX/{include|lib|...}` will be the export location for headers/library/goodies;
        `CMAKE_BUILD_TYPE={Release|RelWithDebInfo|RelMinSize|Debug|}` specifies build config.
        More options are available -- `CMAKE_*` for CMake ones; `CFG_*` for Flow-IPC ones -- and can be
        viewed with `ccmake` or by glancing at `$BUILD/CMakeCache.txt` after running `cmake` or `ccmake`.
        - Regarding `CMAKE_BUILD_TYPE`, you can use the empty "" type to supply
          your own compile/link flags, such as if your organization or product has a standard set suitable for your
          situation.  With the non-blank types we'll take CMake's sensible defaults -- which you can override
          as well.  (See CMake docs; and/or a shortcut is checking out `$BUILD/CMakeCache.txt`.)
        - This is the likeliest stage at which CMake would detect lacking dependencies.  See CMake docs for
          how to tweak its robust dependency-searching behavior; but generally if it's not in a global system
          location, or not in the `CMAKE_INSTALL_PREFIX` (export) location itself, then you can provide more
          search locations by adding a semicolon-separated list thereof via `-DCMAKE_PREFIX_PATH=...`.
        - Alternatively most things' locations can be individually specified via `..._DIR` settings.
     4. Build using the build system generated in the preceding step:  In `$BUILD` run `make`.  
     5. Install (export):  In `$BUILD` run `make install`.  

To use `ipc_session`:

  - `#include` the relevant exported header(s).
  - Link the exported library (such as `libipc_session.a`) and the required other libraries to
    your executable(s).
    - If using CMake to build such executable(s):
      1. Simply use `find_package(IpcSession)` to find it.
      2. Then use `target_link_libraries(... IpcSession::ipc_session)` on your target
         to ensure all necessary libraries are linked.
         (This will include the libraries themselves and the dependency libraries it needs to avoid undefined-reference
         errors when linking.  Details on such things can be found in CMake documentation; and/or you may use
         our CMake script(s) for inspiration; after all we do build all the libraries and a `*_link_test.exec`
         executable.)
    - Otherwise specify it manually based on your build system of choice (if any).  To wit, in order:
      - Link against `libipc_session.a`, `libipc_transport_structured.a`, `libipc_core.a`, and `libflow.a`.
      - Link against Boost libraries mentioned in a `flow/.../CMakeLists.txt` line (search `flow` dependency for it):
        `set(BOOST_LIBS ...)`.
      - Link against the {fmt} library, `libfmt`.
      - Link against the system pthreads library and `librt`.
  - Read the documentation to learn how to use Flow-IPC's (and/or Flow's) various features.
    (See Documentation below.)

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_session` lacks its own generated documentation.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Contributing

See Flow-IPC meta-project's `README.md` Contributing section.
