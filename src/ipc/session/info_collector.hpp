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

/// @file
#pragma once

#include "ipc/session/session_fwd.hpp"
#include "ipc/session/detail/session_fwd.hpp"
#include "ipc/transport/struc/channel_stats.hpp"
#include "ipc/transport/blob_transport_stats.hpp"
#include "ipc/util/util.hpp"
#include <boost/weak_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <cassert>
#include <ostream>

namespace ipc::session
{

// Types.

/**
 * Bundles access to stats from the session master channel (SMC) -- the internal `struc::Channel` each Session uses
 * for protocol traffic (log-in, channel-opening, graceful close, liveness status).  The user obtains an
 * `Info_collector*` via Session::info_collector().
 *
 * Two levels of stats are exposed as of this writing:
 *   - Structured-channel stats (transport::struc::stat::Channel_stats): master_channel_stats(),
 *     master_channel_stats_reset().
 *   - Transport-level stats from the underlying `Native_socket_stream`: master_channel_transport_snd_stats(),
 *     master_channel_transport_snd_stats_reset(), master_channel_transport_rcv_stats(),
 *     master_channel_transport_rcv_stats_reset().
 *
 * One can work in standard `flow::util::stat` ways with each stat-set accessed via `*_stats()`.  This includes
 * regular `struct` member reads (of course), sum/aggregation with others (`flow::util::stats_aggregate()` et al),
 * and printing via `ostream << print(...)`.
 *
 * W/r/t the latter operation -- printing -- it is also possible to print all the `*_stats()` in one shot:
 * simply use `os << *this`.  You may use the simple in knobs (public, mutable) #m_fmt to affect the format of
 * the print-op (e.g.: multi-line versus single-line).
 *
 * @note Slight subtlety: A `*this` is a live-accessor of information, not a store (dump) of information.
 *       It has those `*_reset()` methods, for one thing; but more to the point: It is not a classic `struct`
 *       with public data members storing the stats/info (cf., say, the SHM-providers' `Info_dump` pattern -- e.g.,
 *       shm::classic::Pool_arena::Info_dump).  Each `*_stats()` call will grab a snapshot of those data
 *       as of this moment; they may not equal what it returns a split-second later.  Session::info_collector(),
 *       which returns us, returns a pointer (`this`) and does not itself capture any stats/info.
 *       (Cf., say, shm::classic::Pool_arena::info_dump() which returns a new stats/info-filled object.)
 * @note Corollary: `ostream << *this` shall internally perform all of `*_stats()` info-grabbing ops.
 *       (Cf. `ostream <<` of Pool_arena::Info_dump; it outputs serializations of already-stored stats/info.)
 *
 * ### Subject matter ###
 * The aforementioned SMC (structured master channel) stats track the behavior of an internal facility.  One does
 * not normally concern themselves with these workings; but in short ipc::session relies on a struc::Channel
 * over a transport::Native_socket_stream to establish and end any Session; as well as opening on-demand
 * user `Channel`s throughout, plus liveness detection (so that if the opposing side ends the session, this side
 * will quickly know and report to the user, who should stop that IPC session/start a new one/etc.).  Some
 * potential ways in which stats about this are useful:
 *   - to ascertain that the background traffic exchanged uses negligible resources (versus the user's actual
 *     IPC work which ipc::session makes possible by establishing IPC conversations a/k/a sessions);
 *   - in long-lived processes to get an idea of how many channels are opened when;
 *   - to generally understand what's happening behind the scenes.
 *
 * ### Thread safety ###
 * It is safe to call all methods (`const` or otherwise) at any time from any thread (even though the Session operates
 * the SMC in the background).
 *
 * `ostream << *this` is not formally safe w/r/t a concurrent modification of `*this` #m_fmt.
 *
 * ### Lifetime ###
 * The pointer returned by Session::info_collector() is stable from PEER state (for `Client_session`, after
 * `sync_connect()` returns; for `Server_session` throughout its lifetime once the user obtains access to
 * it via successful Session_server::async_accept() or equivalent).  Before PEER state,
 * `Client_session::info_collector()` returns null.
 *
 * That pointer points to a member of the `Session` and hence must not be dereferenced beyond that `Session`'s
 * life.  However `Info_collector` is copyable, and a copy may safely outlive the source `Session`: once the
 * `Session` (and hence the SMC) is gone -- master_channel_live() returns `false` -- the stat-getters return
 * all-zero values; the resetters no-op; and printing `*this` yields a note that the stats
 * are unavailable.
 *
 * @internal
 *
 * ### Impl notes ###
 * It is quite straightforward, once the situation has gotten to `*this` existing.  The only question that arises,
 * we think, is thread safety: we guarantee above that they can call any of our methods anytime during the lifetime
 * of the generating `Session`.  There are two levels to that:
 *   - Is the saved pointer #m_master_channel pointing to the right thing, and is that thing alive?  Answer:
 *     yes, as by the time either Client_session_impl or Server_session_impl creates a `*this`, *and* the user
 *     has access to the `Session` in the first place: each of those classes internally promises
 *     the pointee SMC data member stays around until the destruction of the `..._impl` and its containing
 *     `Session`.
 *   - Are the actual calls to which we forward safe to call, while the SMC (a struc::Channel) is doing
 *     concurrent work (receiving traffic and such)?  Answer: yes:
 *     - `struc::Channel::stats()` (and `stats_reset()`) internally lock the SMC's own mutex -- advertised as
 *       safe-anytime; we are just another user in that sense.
 *     - struc::Channel::owned_channel() and struc::Channel::owned_channel_mutable() return
 *       `flow::util::Locked_proxy`s instead of raw ptrs/refs; as a result the async-I/O-pattern `Channel`'s
 *       internal mutex is locked through the end of the expression (in our case `...->*_stats[_reset]()`).
 *       We use those (as of this writing there is no alternative/not-locking way in fact).
 *
 * Note the Lifetime section speaks only of Session destruction causing `!master_channel_live()`.  Internally
 * there is also a degenerate window: Server_session_impl's init-channel-opening failure path resets the SMC
 * `shared_ptr` *after* having emplaced its Info_collector member.  However that is not user-reachable: a failed
 * `Session_server::async_accept()` leaves the user's target `Server_session` untouched (empty), so the user
 * never sees the affected `..._impl`.  And even if some future refactor changed that: the graceful
 * `!master_channel_live()` semantics make the window moot anyway.
 *
 * Regarding stat-related `*stats_configure_...()`s, such as on struc::Channel at least: Naively it might make
 * sense to expose/forward those too.  However, as of this writing, that would make no sense: The relevant
 * `Channel`s are already `Channel::start()`ed by the time the user has access to the relevant Session.
 *
 * Lastly regarding #Master_structured_channel, which is a template parameter: The type of the SMC object
 * in Client_session_impl and Server_session_impl is "almost" concrete but does depend on the `typename Mdt_payload`
 * parameter, such as to #Client_session and #Server_session and equivalents, that must be supplied
 * by the user (with a default of `Void`, in the likely case they do not wish to use that feature).  As for what
 * `Mdt_payload` is for: see Session concept.  That topic is irrelevant to us here though.
 *
 * @endinternal
 *
 * @tparam Master_structured_channel_t
 *         This is an implementation detail.  The concrete type for the user is aliased as Session::Info_collector.
 */
template<typename Master_structured_channel_t>
class Info_collector
{
public:
  // Types.

  /// The concrete `struc::Channel` type of the session master channel.
  using Master_structured_channel = Master_structured_channel_t;

  // Data.

  /**
   * Formatting knobs for printing `*this` via `ostream <<`.  util::stat::Info_dump_format is also used by other
   * Flow-IPC code; here `m_multiline` is honored, while `m_verbose` has no effect (there is no verbose-only
   * content to gate).
   */
  util::stat::Info_dump_format m_fmt;

  // Methods.

  /**
   * Returns whether the session master channel still exists; `false` means the source `Session` has been
   * destroyed (`*this` being a copy that outlived it; see class doc header, Lifetime section).  When `false`:
   * the stat-getters return all-zero values; the resetters no-op; printing `*this` yields
   * a note that the stats are unavailable.
   *
   * @return See above.
   */
  bool master_channel_live() const;

  /**
   * struc::Channel::stats() of the session master channel of the Session that spawned `*this`.
   * @return See above; or an all-zero value if `!master_channel_live()`.
   */
  transport::struc::stat::Channel_stats master_channel_stats() const;

  /**
   * struc::Channel::reset_stats() of the session master channel of the Session that spawned `*this`.
   * No-op if `!master_channel_live()`.
   */
  void master_channel_stats_reset();

  /**
   * Native_socket_stream::native_handle_snd_stats() of the transport channel powering the session master
   * channel of the Session that spawned `*this`.
   *
   * @note Despite the aforementioned reference to `native_handle`, the stats `struct` really covers both
   *       (arguably mainly) data being trafficked as well as native handles (which are indeed involved in
   *       various things, notably opening `Native_socket_stream`-containing channels through the Session).
   *       It's an artifact (not that we're saying it's a bad thing) of how concepts interact --
   *       Native_handle_sender versus Blob_sender and so on.
   *
   * @return See above; or an all-zero value if `!master_channel_live()`.
   */
  transport::stat::Blob_snd_stats master_channel_transport_snd_stats() const;

  /**
   * Native_socket_stream::native_handle_snd_stats_reset() of the transport channel powering the session master
   * channel of the Session that spawned `*this`.  No-op if `!master_channel_live()`.
   */
  void master_channel_transport_snd_stats_reset();

  /**
   * Native_socket_stream::native_handle_rcv_stats() of the transport channel powering the session master
   * channel of the Session that spawned `*this`.
   *
   * @note Same note as for master_channel_transport_snd_stats().
   *
   * @return See above; or an all-zero value if `!master_channel_live()`.
   */
  transport::stat::Blob_rcv_stats master_channel_transport_rcv_stats() const;

  /**
   * Native_socket_stream::native_handle_rcv_stats_reset() of the transport channel powering the session master
   * channel of the Session that spawned `*this`.  No-op if `!master_channel_live()`.
   */
  void master_channel_transport_rcv_stats_reset();

private:
  // Friends.

  /// Attorney granting internal code access to at least the private ctor.
  friend struct Info_collector_dtl;

  // Types.

  /// Short-hand for `shared_ptr` to #Master_structured_channel.
  using Master_structured_channel_ptr = boost::shared_ptr<Master_structured_channel>;

  /// Short-hand for `weak_ptr` observer of #Master_structured_channel_ptr.
  using Master_structured_channel_observer = boost::weak_ptr<Master_structured_channel>;

  // Constructors.

  /**
   * Constructs the collector, capturing a `weak_ptr` to the session master channel.
   * Only callable by detail::Info_collector_dtl::ct_base().
   *
   * @param master_channel
   *        The SMC; must be non-null.  A `weak_ptr` observer is stored.
   */
  explicit Info_collector(const Master_structured_channel_ptr& master_channel);

  // Data.

  /**
   * `weak_ptr` to the session master channel.
   *
   * ### Semantics/rationale (why `weak_ptr`?) ###
   * Internally to Client_session_impl and Server_session_impl, the SMC `shared_ptr` is immutable from PEER
   * state through dtor -- even on session hosing the channel object remains alive in both Client_session_impl
   * and Server_session_impl (and their public-facing subclasses).  So `lock()` succeeds while the source
   * `Session` lives (modulo the degenerate never-reached-working-order cases; see class doc header, Lifetime
   * section) and fails once it is destroyed -- which is possible to observe from `*this` being a copy that
   * outlived it.  That distinction is exactly master_channel_live(); the accessors and `ostream <<` use it to
   * behave gracefully (zeroes/no-op/note) rather than access a dead SMC.
   */
  Master_structured_channel_observer m_master_channel;
}; // class Info_collector

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Master_structured_channel_t>
Info_collector<Master_structured_channel_t>::Info_collector(const Master_structured_channel_ptr& master_channel) :
  m_master_channel(master_channel)
{
  assert(master_channel && "Info_collector ctor: master_channel must be non-null.");
}

template<typename Master_structured_channel_t>
bool Info_collector<Master_structured_channel_t>::master_channel_live() const
{
  return !m_master_channel.expired();
}

template<typename Master_structured_channel_t>
transport::struc::stat::Channel_stats Info_collector<Master_structured_channel_t>::master_channel_stats() const
{
  using transport::struc::stat::Channel_stats;

  const auto master_channel = m_master_channel.lock();
  return master_channel ? master_channel->stats() : Channel_stats{};
}

template<typename Master_structured_channel_t>
void Info_collector<Master_structured_channel_t>::master_channel_stats_reset()
{
  const auto master_channel = m_master_channel.lock();
  if (master_channel)
  {
    master_channel->stats_reset();
  }
}

template<typename Master_structured_channel_t>
transport::stat::Blob_snd_stats Info_collector<Master_structured_channel_t>::master_channel_transport_snd_stats() const
{
  using transport::stat::Blob_snd_stats;

  const auto master_channel = m_master_channel.lock();
  return master_channel ? master_channel->owned_channel()->native_handle_send_stats()
                        : Blob_snd_stats{1};
}

template<typename Master_structured_channel_t>
void Info_collector<Master_structured_channel_t>::master_channel_transport_snd_stats_reset()
{
  const auto master_channel = m_master_channel.lock();
  if (master_channel)
  {
    master_channel->owned_channel_mutable()->native_handle_send_stats_reset();
  }
}

template<typename Master_structured_channel_t>
transport::stat::Blob_rcv_stats Info_collector<Master_structured_channel_t>::master_channel_transport_rcv_stats() const
{
  using transport::stat::Blob_rcv_stats;

  const auto master_channel = m_master_channel.lock();
  return master_channel ? master_channel->owned_channel()->native_handle_receive_stats()
                        : Blob_rcv_stats{1};
}

template<typename Master_structured_channel_t>
void Info_collector<Master_structured_channel_t>::master_channel_transport_rcv_stats_reset()
{
  const auto master_channel = m_master_channel.lock();
  if (master_channel)
  {
    master_channel->owned_channel_mutable()->native_handle_receive_stats_reset();
  }
}

template<typename Master_structured_channel_t>
std::ostream& operator<<(std::ostream& os, const Info_collector<Master_structured_channel_t>& val)
{
  using util::String_view;
  using flow::util::stat::print;

  if (!val.master_channel_live())
  {
    // Per contract: the source Session is gone (or never reached working order); no stats exist to show.
    return os << "[session master channel gone; no stats avail]";
  }
  // else

  String_view ln; // Top-level-item separator.
  if (val.m_fmt.m_multiline)
  {
    os << "- ";
    ln = "\n- ";
  }
  else
  {
    ln = " | ";
  }

  return os << "smc-struc: [" << print(val.master_channel_stats()) << ']' << ln
            << "smc-transport-snd: [" << print(val.master_channel_transport_snd_stats()) << ']' << ln
            << "smc-transport-rcv: [" << print(val.master_channel_transport_rcv_stats()) << ']';
  // (Intentional: no newline at the end.)
} // operator<<(ostream&, Info_collector)

} // namespace ipc::session
