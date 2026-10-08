// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ppp.hpp
 * @brief `structo::net::ppp_link`: a sans-IO PPP control-protocol engine
 * (LCP, RFC 1661, and IPCP, RFC 1332/1877) for the client side of a serial
 * link. C++20 only.
 *
 * It decodes nothing and sends nothing by itself: you feed it received
 * frames (`on_frame`) and ask it for control frames to transmit
 * (`next_packet`), passing a millisecond timestamp so it can retransmit
 * (Restart timer, default 3 s, at most 10 Configure-Requests). `hw::ppp_device`
 * does exactly that over a UART.
 *
 * What is negotiated:
 * - **LCP**: we ask for nothing special (empty Configure-Request); the
 *   peer's MRU, ACCM, magic number, PFC and ACFC are acknowledged,
 *   everything else (notably *authentication*) is rejected. Echo-Requests
 *   are answered; unknown protocols get a Protocol-Reject.
 * - **IPCP**: our IP-Address option asks for `ppp_config::local_address`
 *   (0.0.0.0 = "assign me one"); a Nak from the peer is adopted. The peer's
 *   address is acknowledged (if it asks us to assign one, `peer_address` is
 *   used). Primary DNS (RFC 1877) is requested when `request_dns` is set.
 *   IPv4 packets (protocol 0x0021) are *not* consumed here.
 *
 * There is no authentication (PAP/CHAP): use `noauth` on the peer.
 *
 * @code
 * structo::net::ppp_config cfg;       // defaults: ask the peer for our address and DNS
 * structo::net::ppp_link link{cfg};
 * link.open();                        // start negotiating on the next next_packet()/on_frame()
 *
 * // For every received PPP frame (protocol number + information field):
 * bool consumed = link.on_frame(frame.protocol, frame.payload, now_ms);
 * // consumed == false for IPv4 (0x0021): hand frame.payload to the IP layer, if link.ip_up().
 *
 * // Whenever the line can take a frame: ask for the next control frame to send.
 * if (auto pkt = link.next_packet(now_ms)) {
 *   // pkt->protocol, pkt->data (valid until the next call): encode and transmit.
 * }
 * if (link.ip_up()) { // IPCP opened: link.local_address(), link.peer_address(), link.dns()
 * }
 * @endcode
 */

#include "ipv4_config.hpp"

#if RELOCO_HAS_COROUTINES

#include "../hw/ppp_framing.hpp"

#include <reloco/array.hpp>

namespace structo::net {

/** @brief Static settings of a `ppp_link`. */
struct ppp_config {
  ipv4_address local_address{}; ///< Address to request for us; 0.0.0.0 = let the peer choose.
  ipv4_address peer_address{};  ///< Address to give the peer if it asks us to assign one; 0.0.0.0 = refuse.
  bool request_dns = true;      ///< Ask the peer for its primary DNS server.
  std::uint16_t mru = 0;        ///< MRU to announce in LCP (what we can receive); 0 = don't send (peer assumes 1500).
  std::uint32_t restart_ms = 3000;
  unsigned max_configure = 10;  ///< Configure-Requests sent before giving up.
  unsigned max_terminate = 2;   ///< Terminate-Requests sent before giving up.
};

/** @brief State of one control protocol (RFC 1661 section 4, simplified). */
enum class ppp_state : std::uint8_t { closed, req_sent, ack_rcvd, ack_sent, opened, closing };

/** @brief A control frame to transmit; `data` is valid until the next `next_packet()`. */
struct ppp_packet {
  std::uint16_t protocol = 0;
  reloco::span<const std::uint8_t> data;
};

class ppp_link {
public:
  explicit ppp_link(const ppp_config &cfg = {}) noexcept
      : cfg_(cfg), local_(cfg.local_address), mru_(cfg.mru), want_dns_(cfg.request_dns), send_mru_(cfg.mru != 0) {
    lcp_.proto = hw::ppp_proto_lcp;
    ipcp_.proto = hw::ppp_proto_ipcp;
  }

  /** @brief Administratively opens the link; negotiation starts with the next `next_packet()`/`on_frame()`. */
  void open() noexcept {
    admin_open_ = true;
    failed_ = false;
  }

  /** @brief Administratively closes the link; a Terminate-Request is sent if LCP was open. */
  void close(std::uint64_t now_ms) noexcept {
    now_ = now_ms;
    admin_open_ = false;
    if (ipcp_.st != ppp_state::closed)
      layer_down(ipcp_);
    if (lcp_.st == ppp_state::opened) {
      layer_down(lcp_);
      lcp_.retries = 0;
      send_terminate_request(lcp_);
      lcp_.st = ppp_state::closing;
    } else {
      lcp_.st = ppp_state::closed;
      lcp_.timer = false;
    }
  }

  [[nodiscard]] ppp_state lcp_state() const noexcept { return lcp_.st; }
  [[nodiscard]] ppp_state ipcp_state() const noexcept { return ipcp_.st; }
  /** @brief True once IPCP is open and we have an address: IPv4 packets may flow. */
  [[nodiscard]] bool ip_up() const noexcept { return ipcp_.st == ppp_state::opened && !local_.is_unspecified(); }
  /** @brief Negotiation gave up (retries exhausted or the peer rejected IPCP); call `open()` to try again. */
  [[nodiscard]] bool failed() const noexcept { return failed_; }

  [[nodiscard]] ipv4_address local_address() const noexcept { return local_; }
  [[nodiscard]] ipv4_address peer_address() const noexcept { return peer_; }
  [[nodiscard]] ipv4_address dns() const noexcept { return dns_; }
  /** @brief The negotiated addressing as an `ipv4_config` (peer = gateway, /32 netmask). */
  [[nodiscard]] ipv4_config ipv4() const noexcept {
    ipv4_config c;
    c.address = local_;
    c.netmask = ipv4_address{255, 255, 255, 255};
    c.gateway = peer_;
    c.dns = dns_;
    return c;
  }
  /** @brief Largest information field the peer accepts (its MRU; 1500 until it says otherwise). */
  [[nodiscard]] std::uint16_t peer_mru() const noexcept { return peer_mru_; }

  /**
   * @brief Processes one received frame. Returns true if it was a control
   * frame (consumed), false for IPv4 (0x0021), which the caller must handle.
   */
  bool on_frame(std::uint16_t protocol, reloco::span<const std::uint8_t> payload, std::uint64_t now_ms) noexcept {
    now_ = now_ms;
    ensure_started();
    if (protocol == hw::ppp_proto_ip)
      return false;
    if (protocol == hw::ppp_proto_lcp) {
      on_control(lcp_, payload);
    } else if (protocol == hw::ppp_proto_ipcp) {
      if (lcp_.st == ppp_state::opened)
        on_control(ipcp_, payload);
    } else if (lcp_.st == ppp_state::opened) {
      send_protocol_reject(protocol, payload);
    }
    return true;
  }

  /** @brief Next control frame to transmit, or `error::try_again`. Also runs the retransmission timers. */
  [[nodiscard]] reloco::result<ppp_packet> next_packet(std::uint64_t now_ms) noexcept {
    now_ = now_ms;
    ensure_started();
    run_timer(lcp_);
    run_timer(ipcp_);
    if (out_count_ == 0)
      return reloco::unexpected(reloco::error::try_again);
    const slot &s = out_[out_head_];
    current_ = s; // keep the data alive after the slot is recycled
    out_head_ = (out_head_ + 1) % out_slots;
    --out_count_;
    return ppp_packet{current_.proto, reloco::span<const std::uint8_t>(current_.data.data(), current_.len)};
  }

private:
  static constexpr std::size_t out_slots = 4;
  static constexpr std::size_t slot_size = 96; // header + options; larger replies are dropped

  enum : std::uint8_t {
    code_conf_request = 1,
    code_conf_ack = 2,
    code_conf_nak = 3,
    code_conf_reject = 4,
    code_term_request = 5,
    code_term_ack = 6,
    code_code_reject = 7,
    code_protocol_reject = 8,
    code_echo_request = 9,
    code_echo_reply = 10,
    code_discard_request = 11
  };

  struct layer {
    std::uint16_t proto = 0;
    ppp_state st = ppp_state::closed;
    std::uint8_t next_id = 0;
    std::uint8_t req_id = 0;
    unsigned retries = 0;
    std::uint64_t deadline = 0;
    bool timer = false;
  };

  struct slot {
    std::uint16_t proto = 0;
    std::size_t len = 0;
    reloco::array<std::uint8_t, slot_size> data{};
  };

  using bytes = reloco::span<const std::uint8_t>;

  // ---- output ----------------------------------------------------------

  bool enqueue(std::uint16_t proto, std::uint8_t code, std::uint8_t id, bytes body) noexcept {
    if (out_count_ == out_slots || body.size() + 4 > slot_size)
      return false;
    slot &s = out_[(out_head_ + out_count_) % out_slots];
    s.proto = proto;
    s.len = body.size() + 4;
    s.data[0] = code;
    s.data[1] = id;
    s.data[2] = static_cast<std::uint8_t>(s.len >> 8);
    s.data[3] = static_cast<std::uint8_t>(s.len);
    for (std::size_t i = 0; i < body.size(); ++i)
      s.data[4 + i] = body[i];
    ++out_count_;
    return true;
  }

  static void put_option_ip(reloco::array<std::uint8_t, slot_size> &buf, std::size_t &n, std::uint8_t type,
                            ipv4_address a) noexcept {
    buf[n++] = type;
    buf[n++] = 6;
    for (std::uint8_t o : a.octets)
      buf[n++] = o;
  }

  void send_cr(layer &l) noexcept {
    if (l.retries >= cfg_.max_configure) {
      fail(l);
      return;
    }
    ++l.retries;
    reloco::array<std::uint8_t, slot_size> opts{};
    std::size_t n = 0;
    if (&l == &lcp_ && send_mru_) {
      opts[n++] = 1;
      opts[n++] = 4;
      opts[n++] = static_cast<std::uint8_t>(mru_ >> 8);
      opts[n++] = static_cast<std::uint8_t>(mru_);
    }
    if (&l == &ipcp_) {
      if (send_ip_option_)
        put_option_ip(opts, n, 3, local_);
      if (want_dns_)
        put_option_ip(opts, n, 129, dns_);
    }
    l.req_id = ++l.next_id;
    (void)enqueue(l.proto, code_conf_request, l.req_id, bytes(opts.data(), n));
    l.timer = true;
    l.deadline = now_ + cfg_.restart_ms;
  }

  void send_terminate_request(layer &l) noexcept {
    ++l.retries;
    l.req_id = ++l.next_id;
    (void)enqueue(l.proto, code_term_request, l.req_id, {});
    l.timer = true;
    l.deadline = now_ + cfg_.restart_ms;
  }

  void send_protocol_reject(std::uint16_t protocol, bytes info) noexcept {
    reloco::array<std::uint8_t, slot_size> body{};
    body[0] = static_cast<std::uint8_t>(protocol >> 8);
    body[1] = static_cast<std::uint8_t>(protocol);
    std::size_t n = 2;
    for (std::size_t i = 0; i < info.size() && n + 4 < slot_size; ++i)
      body[n++] = info[i];
    (void)enqueue(hw::ppp_proto_lcp, code_protocol_reject, ++lcp_.next_id, bytes(body.data(), n));
  }

  // ---- layer transitions ---------------------------------------------

  void ensure_started() noexcept {
    if (admin_open_ && lcp_.st == ppp_state::closed && !lcp_.timer && !failed_) {
      lcp_.retries = 0;
      send_cr(lcp_);
      if (!failed_)
        lcp_.st = ppp_state::req_sent;
    }
  }

  void fail(layer &l) noexcept {
    l.st = ppp_state::closed;
    l.timer = false;
    if (&l == &lcp_ && ipcp_.st != ppp_state::closed)
      layer_down(ipcp_);
    failed_ = true;
    admin_open_ = false;
  }

  // "This layer up".
  void layer_up(layer &l) noexcept {
    l.retries = 0;
    l.timer = false;
    if (&l == &lcp_) {
      ipcp_.retries = 0;
      send_ip_option_ = true;
      send_cr(ipcp_);
      if (!failed_)
        ipcp_.st = ppp_state::req_sent;
    }
  }

  // "This layer down".
  void layer_down(layer &l) noexcept {
    if (&l == &lcp_) {
      if (ipcp_.st != ppp_state::closed)
        layer_down(ipcp_);
    } else {
      peer_ = {};
      ipcp_.st = ppp_state::closed;
      ipcp_.timer = false;
    }
  }

  void run_timer(layer &l) noexcept {
    if (!l.timer || now_ < l.deadline)
      return;
    switch (l.st) {
    case ppp_state::closing:
      if (l.retries >= cfg_.max_terminate) {
        l.st = ppp_state::closed;
        l.timer = false;
      } else {
        send_terminate_request(l);
      }
      break;
    case ppp_state::req_sent:
    case ppp_state::ack_rcvd:
    case ppp_state::ack_sent:
      send_cr(l);
      if (!failed_ && l.st == ppp_state::ack_rcvd)
        l.st = ppp_state::req_sent;
      break;
    default:
      l.timer = false;
      break;
    }
  }

  // ---- packet processing -----------------------------------------------

  void on_control(layer &l, bytes p) noexcept {
    if (p.size() < 4)
      return;
    const std::uint8_t code = p[0];
    const std::uint8_t id = p[1];
    const std::size_t len = (static_cast<std::size_t>(p[2]) << 8) | p[3];
    if (len < 4 || len > p.size())
      return;
    const bytes body(p.data() + 4, len - 4);
    switch (code) {
    case code_conf_request:
      on_conf_request(l, id, body);
      break;
    case code_conf_ack:
      on_conf_ack(l, id);
      break;
    case code_conf_nak:
    case code_conf_reject:
      on_conf_nak_reject(l, id, code, body);
      break;
    case code_term_request:
      (void)enqueue(l.proto, code_term_ack, id, {});
      if (l.st == ppp_state::opened)
        layer_down(l);
      l.st = ppp_state::closed;
      l.timer = false;
      if (&l == &lcp_ && ipcp_.st != ppp_state::closed)
        layer_down(ipcp_);
      break;
    case code_term_ack:
      if (l.st == ppp_state::closing) {
        l.st = ppp_state::closed;
        l.timer = false;
      } else if (l.st == ppp_state::opened) {
        layer_down(l);
        l.retries = 0;
        send_cr(l);
        l.st = ppp_state::req_sent;
      } else if (l.st == ppp_state::ack_rcvd) {
        l.st = ppp_state::req_sent;
      }
      break;
    case code_echo_request:
      if (&l == &lcp_ && l.st == ppp_state::opened) {
        reloco::array<std::uint8_t, slot_size> reply{};
        std::size_t n = 0;
        for (; n < body.size() && n + 8 < slot_size; ++n)
          reply[n] = n < 4 ? std::uint8_t{0} : body[n]; // our magic number is 0 (not negotiated)
        (void)enqueue(l.proto, code_echo_reply, id, bytes(reply.data(), n));
      }
      break;
    case code_protocol_reject:
      if (&l == &lcp_ && body.size() >= 2 && ((body[0] << 8) | body[1]) == hw::ppp_proto_ipcp) {
        layer_down(ipcp_);
        failed_ = true; // the peer does not do IP
      }
      break;
    case code_echo_reply:
    case code_discard_request:
      break;
    default: {
      reloco::array<std::uint8_t, slot_size> copy{};
      std::size_t n = 0;
      for (; n < p.size() && n + 4 < slot_size; ++n)
        copy[n] = p[n];
      (void)enqueue(l.proto, code_code_reject, ++l.next_id, bytes(copy.data(), n));
      break;
    }
    }
  }

  enum class verdict : std::uint8_t { ack, nak, reject, malformed };

  // Judges the peer's options; fills nak_/rej_ with the options to send back.
  verdict judge(const layer &l, bytes opts) noexcept {
    nak_len_ = rej_len_ = 0;
    pending_mru_ = 1500;
    pending_peer_ = {};
    std::size_t pos = 0;
    while (pos < opts.size()) {
      if (pos + 2 > opts.size())
        return verdict::malformed;
      const std::uint8_t type = opts[pos];
      const std::size_t olen = opts[pos + 1];
      if (olen < 2 || pos + olen > opts.size())
        return verdict::malformed;
      const bytes o(opts.data() + pos, olen);
      pos += olen;
      if (&l == &lcp_) {
        const bool known = (type == 1 && olen == 4) || (type == 2 && olen == 6) || (type == 5 && olen == 6) ||
                           (type == 7 && olen == 2) || (type == 8 && olen == 2);
        if (!known)
          add(rej_, rej_len_, o);
        else if (type == 1)
          pending_mru_ = static_cast<std::uint16_t>((o[2] << 8) | o[3]);
      } else if (type == 3 && olen == 6) {
        const ipv4_address a{o[2], o[3], o[4], o[5]};
        if (!a.is_unspecified()) {
          pending_peer_ = a;
        } else if (!cfg_.peer_address.is_unspecified()) {
          const std::uint8_t nk[6] = {3, 6, cfg_.peer_address.octets[0], cfg_.peer_address.octets[1],
                                      cfg_.peer_address.octets[2], cfg_.peer_address.octets[3]};
          add(nak_, nak_len_, bytes(nk, 6));
        } else {
          add(rej_, rej_len_, o);
        }
      } else {
        add(rej_, rej_len_, o);
      }
    }
    return rej_len_ ? verdict::reject : nak_len_ ? verdict::nak : verdict::ack;
  }

  static void add(reloco::array<std::uint8_t, slot_size> &buf, std::size_t &n, bytes o) noexcept {
    if (n + o.size() + 4 > slot_size)
      return;
    for (std::uint8_t b : o)
      buf[n++] = b;
  }

  void on_conf_request(layer &l, std::uint8_t id, bytes opts) noexcept {
    if (l.st == ppp_state::closing)
      return;
    if (l.st == ppp_state::closed) {
      (void)enqueue(l.proto, code_term_ack, id, {});
      return;
    }
    const verdict v = judge(l, opts);
    if (v == verdict::malformed)
      return;
    const bool good = v == verdict::ack;
    if (v == verdict::ack)
      (void)enqueue(l.proto, code_conf_ack, id, opts);
    else if (v == verdict::nak)
      (void)enqueue(l.proto, code_conf_nak, id, bytes(nak_.data(), nak_len_));
    else
      (void)enqueue(l.proto, code_conf_reject, id, bytes(rej_.data(), rej_len_));
    if (good) {
      if (&l == &lcp_)
        peer_mru_ = pending_mru_;
      else
        peer_ = pending_peer_;
    }
    switch (l.st) {
    case ppp_state::req_sent:
      if (good)
        l.st = ppp_state::ack_sent;
      break;
    case ppp_state::ack_rcvd:
      if (good) {
        l.st = ppp_state::opened;
        layer_up(l);
      }
      break;
    case ppp_state::ack_sent:
      if (!good)
        l.st = ppp_state::req_sent;
      break;
    case ppp_state::opened:
      layer_down(l);
      l.retries = 0;
      send_cr(l);
      l.st = good ? ppp_state::ack_sent : ppp_state::req_sent;
      break;
    default:
      break;
    }
  }

  void on_conf_ack(layer &l, std::uint8_t id) noexcept {
    if (id != l.req_id)
      return;
    switch (l.st) {
    case ppp_state::req_sent:
      l.st = ppp_state::ack_rcvd;
      l.retries = 0;
      break;
    case ppp_state::ack_rcvd: // crossed: ask again
      send_cr(l);
      l.st = ppp_state::req_sent;
      break;
    case ppp_state::ack_sent:
      l.st = ppp_state::opened;
      layer_up(l);
      break;
    case ppp_state::opened:
      layer_down(l);
      l.retries = 0;
      send_cr(l);
      l.st = ppp_state::req_sent;
      break;
    default:
      break;
    }
  }

  void on_conf_nak_reject(layer &l, std::uint8_t id, std::uint8_t code, bytes opts) noexcept {
    if (id != l.req_id || l.st == ppp_state::closed || l.st == ppp_state::closing)
      return;
    {
      std::size_t pos = 0;
      while (pos + 2 <= opts.size()) {
        const std::uint8_t type = opts[pos];
        const std::size_t olen = opts[pos + 1];
        if (olen < 2 || pos + olen > opts.size())
          break;
        if (&l == &lcp_) {
          if (type == 1 && code == code_conf_reject)
            send_mru_ = false;
          else if (type == 1 && olen == 4) {
            const auto v = static_cast<std::uint16_t>((opts[pos + 2] << 8) | opts[pos + 3]);
            if (v >= 128 && v <= cfg_.mru)
              mru_ = v; // the peer suggests a smaller MRU than ours
          }
        } else if (code == code_conf_nak && olen == 6) {
          const ipv4_address a{opts[pos + 2], opts[pos + 3], opts[pos + 4], opts[pos + 5]};
          if (type == 3)
            local_ = a;
          else if (type == 129)
            dns_ = a;
        } else if (code == code_conf_reject) {
          if (type == 3)
            send_ip_option_ = false;
          else if (type == 129)
            want_dns_ = false;
        }
        pos += olen;
      }
    }
    if (l.st == ppp_state::opened)
      layer_down(l);
    send_cr(l);
    if (!failed_ && (l.st == ppp_state::ack_rcvd || l.st == ppp_state::opened))
      l.st = ppp_state::req_sent;
  }

  ppp_config cfg_;
  layer lcp_;
  layer ipcp_;
  ipv4_address local_;
  ipv4_address peer_;
  ipv4_address dns_;
  ipv4_address pending_peer_;
  std::uint16_t peer_mru_ = 1500;
  std::uint16_t pending_mru_ = 1500;
  std::uint16_t mru_;
  bool want_dns_;
  bool send_mru_;
  bool send_ip_option_ = true;
  bool admin_open_ = false;
  bool failed_ = false;
  std::uint64_t now_ = 0;

  reloco::array<slot, out_slots> out_{};
  std::size_t out_head_ = 0;
  std::size_t out_count_ = 0;
  slot current_{};
  reloco::array<std::uint8_t, slot_size> nak_{};
  reloco::array<std::uint8_t, slot_size> rej_{};
  std::size_t nak_len_ = 0;
  std::size_t rej_len_ = 0;
};

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
