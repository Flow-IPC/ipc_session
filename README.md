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

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_session` lacks its own generated documentation.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Obtaining the source code

- As a tarball/zip: The [project web site](https://flow-ipc.github.io) links to individual releases with notes, docs,
  download links.  We are included in a subdirectory off the Flow-IPC root.
- For Git access:
  - `git clone --recurse-submodules git@github.com:Flow-IPC/ipc.git`; or
  - `git clone git@github.com:Flow-IPC/ipc_session.git`

## Installation

See [INSTALL](./INSTALL.md) guide.

## Contributing

See [CONTRIBUTING](./CONTRIBUTING.md) guide.
