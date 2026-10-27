// harmonyrecomp netplay - LAN lobby service.

#include "lobby.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace harmony::netplay::lobby {
namespace {

constexpr uint32_t kMagic = 0x48524E50;  // "HRNP"
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxDatagram = 1400;

enum class Type : uint8_t {
  kSearchRequest = 1,
  kSearchResponse = 2,
  kQosRequest = 3,
  kQosResponse = 4,
};

uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---- Serialization (big-endian) --------------------------------------------

class Writer {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v) {
    u8(static_cast<uint8_t>(v >> 8));
    u8(static_cast<uint8_t>(v));
  }
  void u32(uint32_t v) {
    u16(static_cast<uint16_t>(v >> 16));
    u16(static_cast<uint16_t>(v));
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
  }
  void bytes(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  size_t size() const { return buf_.size(); }
  const std::vector<uint8_t>& data() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  uint8_t u8() {
    if (!need(1)) return 0;
    return p_[pos_++];
  }
  uint16_t u16() {
    uint16_t hi = u8();
    return static_cast<uint16_t>((hi << 8) | u8());
  }
  uint32_t u32() {
    uint32_t hi = u16();
    return (hi << 16) | u16();
  }
  uint64_t u64() {
    uint64_t hi = u32();
    return (hi << 32) | u32();
  }
  bool bytes(void* out, size_t n) {
    if (!need(n)) return false;
    std::copy_n(p_ + pos_, n, static_cast<uint8_t*>(out));
    pos_ += n;
    return true;
  }

 private:
  bool need(size_t n) {
    if (!ok_ || pos_ + n > n_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

struct Header {
  Type type{};
  uint32_t title_id = 0;
  uint64_t instance = 0;
  uint32_t request_id = 0;
};

void WriteHeader(Writer& w, const Header& h) {
  w.u32(kMagic);
  w.u8(kVersion);
  w.u8(static_cast<uint8_t>(h.type));
  w.u32(h.title_id);
  w.u64(h.instance);
  w.u32(h.request_id);
}

bool ReadHeader(Reader& r, Header* h) {
  if (r.u32() != kMagic || r.u8() != kVersion) return false;
  h->type = static_cast<Type>(r.u8());
  h->title_id = r.u32();
  h->instance = r.u64();
  h->request_id = r.u32();
  return r.ok();
}

void WriteAd(Writer& w, const SessionAd& ad) {
  w.u64(ad.id);
  w.bytes(ad.key, sizeof(ad.key));
  w.u32(ad.flags);
  w.u32(ad.max_public);
  w.u32(ad.max_private);
  w.u32(ad.filled_public);
  w.u32(ad.filled_private);
  w.u32(ad.state);
  std::string name = ad.host_name.substr(0, 32);
  w.u8(static_cast<uint8_t>(name.size()));
  w.bytes(name.data(), name.size());
  size_t ctx_count = std::min<size_t>(ad.contexts.size(), 32);
  w.u8(static_cast<uint8_t>(ctx_count));
  for (size_t i = 0; i < ctx_count; ++i) {
    w.u32(ad.contexts[i].id);
    w.u32(ad.contexts[i].value);
  }
  // Properties are optional extras: keep the datagram below kMaxDatagram.
  std::vector<const Property*> props;
  size_t budget = kMaxDatagram > w.size() + 64 ? kMaxDatagram - w.size() - 64 : 0;
  for (const auto& p : ad.properties) {
    size_t cost = 6 + p.value.size();
    if (props.size() >= 32 || p.value.size() > 0xFFFF || cost > budget) continue;
    budget -= cost;
    props.push_back(&p);
  }
  w.u8(static_cast<uint8_t>(props.size()));
  for (const auto* p : props) {
    w.u32(p->id);
    w.u16(static_cast<uint16_t>(p->value.size()));
    w.bytes(p->value.data(), p->value.size());
  }
}

bool ReadAd(Reader& r, SessionAd* ad) {
  ad->id = r.u64();
  r.bytes(ad->key, sizeof(ad->key));
  ad->flags = r.u32();
  ad->max_public = r.u32();
  ad->max_private = r.u32();
  ad->filled_public = r.u32();
  ad->filled_private = r.u32();
  ad->state = r.u32();
  uint8_t name_len = r.u8();
  ad->host_name.resize(name_len);
  if (name_len) r.bytes(ad->host_name.data(), name_len);
  uint8_t ctx_count = r.u8();
  ad->contexts.resize(ctx_count);
  for (auto& c : ad->contexts) {
    c.id = r.u32();
    c.value = r.u32();
  }
  uint8_t prop_count = r.u8();
  ad->properties.resize(prop_count);
  for (auto& p : ad->properties) {
    p.id = r.u32();
    uint16_t len = r.u16();
    p.value.resize(len);
    if (len) r.bytes(p.value.data(), len);
  }
  return r.ok();
}

// ---- Service state ----------------------------------------------------------

struct QosListen {
  bool enabled = false;
  std::vector<uint8_t> data;
};

struct Pending {
  std::mutex mutex;
  std::condition_variable cv;
  uint64_t sent_at = 0;
  std::vector<Found> found;
  std::optional<QosResult> qos;
  std::optional<uint64_t> qos_session;
};

struct State {
  std::mutex mutex;
  Config config;
  udp::Socket socket;
  std::thread thread;
  std::atomic<bool> running{false};
  uint64_t instance = 0;
  std::atomic<uint32_t> next_request{1};
  std::map<uint64_t, SessionAd> ads;
  std::map<uint64_t, QosListen> qos;
  std::unordered_map<uint32_t, std::shared_ptr<Pending>> pending;
  // Lobby service port per host IP, learned from search answers.
  std::unordered_map<uint32_t, uint16_t> host_ports;
};

State& S() {
  static State state;
  return state;
}

LogFn g_log = nullptr;
void Log(const std::string& message) {
  if (g_log) g_log(message);
}

void SendTo(const udp::Endpoint& to, const Writer& w) {
  S().socket.SendTo(to, w.data().data(), w.size());
}

void HandleDatagram(const uint8_t* data, size_t size, const udp::Endpoint& from) {
  auto& s = S();
  Reader r(data, size);
  Header h;
  if (!ReadHeader(r, &h) || h.title_id != s.config.title_id) return;
  if (h.instance == s.instance) return;  // our own broadcast

  switch (h.type) {
    case Type::kSearchRequest: {
      uint8_t flags = r.u8();
      uint64_t wanted = r.u64();
      if (!r.ok()) return;
      std::vector<SessionAd> ads;
      {
        std::lock_guard lock(s.mutex);
        for (const auto& [id, ad] : s.ads) {
          if (!(flags & 1) || id == wanted) ads.push_back(ad);
        }
      }
      for (const auto& ad : ads) {
        Writer w;
        WriteHeader(w, {Type::kSearchResponse, s.config.title_id, s.instance, h.request_id});
        WriteAd(w, ad);
        SendTo(from, w);
      }
      return;
    }
    case Type::kQosRequest: {
      uint64_t session = r.u64();
      uint64_t echo = r.u64();
      if (!r.ok()) return;
      QosListen listen;
      {
        std::lock_guard lock(s.mutex);
        auto it = s.qos.find(session);
        if (it == s.qos.end()) return;  // not listening: no answer, like real XNet
        listen = it->second;
      }
      Writer w;
      WriteHeader(w, {Type::kQosResponse, s.config.title_id, s.instance, h.request_id});
      w.u64(session);
      w.u64(echo);
      w.u8(listen.enabled ? 1 : 0);
      size_t len = listen.enabled ? std::min<size_t>(listen.data.size(), 1024) : 0;
      w.u16(static_cast<uint16_t>(len));
      if (len) w.bytes(listen.data.data(), len);
      SendTo(from, w);
      return;
    }
    case Type::kSearchResponse:
    case Type::kQosResponse: {
      std::shared_ptr<Pending> pending;
      {
        std::lock_guard lock(s.mutex);
        auto it = s.pending.find(h.request_id);
        if (it == s.pending.end()) return;
        pending = it->second;
      }
      uint32_t rtt = static_cast<uint32_t>(NowMs() - pending->sent_at);
      if (h.type == Type::kSearchResponse) {
        Found f;
        if (!ReadAd(r, &f.ad)) return;
        f.from = from;
        f.rtt_ms = rtt;
        {
          std::lock_guard lock(s.mutex);
          s.host_ports[from.ip] = from.port;
        }
        std::lock_guard lock(pending->mutex);
        bool dup = std::any_of(pending->found.begin(), pending->found.end(),
                               [&](const Found& x) { return x.ad.id == f.ad.id; });
        if (!dup) pending->found.push_back(std::move(f));
      } else {
        uint64_t session = r.u64();
        r.u64();  // echo
        QosResult q;
        q.contacted = true;
        q.enabled = r.u8() != 0;
        uint16_t len = r.u16();
        q.data.resize(len);
        if (len) r.bytes(q.data.data(), len);
        if (!r.ok()) return;
        q.rtt_ms = std::max<uint32_t>(rtt, 1);
        std::lock_guard lock(pending->mutex);
        if (pending->qos_session && *pending->qos_session == session && !pending->qos) {
          pending->qos = std::move(q);
        }
      }
      pending->cv.notify_all();
      return;
    }
  }
}

void ServiceLoop() {
  auto& s = S();
  std::vector<uint8_t> buffer(2048);
  while (s.running) {
    udp::Endpoint from;
    int n = s.socket.RecvFrom(buffer.data(), buffer.size(), &from, 100);
    if (n > 0) HandleDatagram(buffer.data(), static_cast<size_t>(n), from);
  }
}

std::shared_ptr<Pending> NewPending(uint32_t* request_id) {
  auto& s = S();
  auto p = std::make_shared<Pending>();
  *request_id = s.next_request++;
  std::lock_guard lock(s.mutex);
  s.pending[*request_id] = p;
  return p;
}

void DropPending(uint32_t request_id) {
  auto& s = S();
  std::lock_guard lock(s.mutex);
  s.pending.erase(request_id);
}

std::vector<udp::Endpoint> SearchTargets() {
  auto& s = S();
  std::vector<udp::Endpoint> targets;
  auto add = [&](udp::Endpoint ep) {
    if (std::find(targets.begin(), targets.end(), ep) == targets.end()) targets.push_back(ep);
  };
  add({0xFFFFFFFFu, s.config.remote_port});
  for (const auto& itf : udp::ListInterfaces()) {
    if (itf.netmask && itf.netmask != 0xFFFFFFFFu) add({itf.broadcast(), s.config.remote_port});
  }
  for (const auto& peer : s.config.peers) add(peer);
  return targets;
}

}  // namespace

void SetLogger(LogFn fn) { g_log = fn; }

bool Start(const Config& config, std::string* error) {
  auto& s = S();
  if (s.running) return true;
  s.config = config;
  if (!s.socket.Open(config.port, error)) return false;
  std::random_device rd;
  s.instance = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ NowMs();
  s.running = true;
  s.thread = std::thread(ServiceLoop);
  Log("lobby service on UDP port " + std::to_string(s.socket.local_port()));
  return true;
}

void Stop() {
  auto& s = S();
  if (!s.running) return;
  s.running = false;
  if (s.thread.joinable()) s.thread.join();
  s.socket.Close();
}

bool Running() { return S().running; }
uint16_t BoundPort() { return S().socket.local_port(); }

void Advertise(const SessionAd& ad) {
  std::lock_guard lock(S().mutex);
  S().ads[ad.id] = ad;
}

void Withdraw(uint64_t session_id) {
  std::lock_guard lock(S().mutex);
  S().ads.erase(session_id);
}

void SetQosListen(uint64_t session_id, std::optional<bool> enabled,
                  const std::vector<uint8_t>* data) {
  std::lock_guard lock(S().mutex);
  auto& q = S().qos[session_id];
  if (enabled) q.enabled = *enabled;
  if (data) q.data = *data;
}

void ReleaseQosListen(uint64_t session_id) {
  std::lock_guard lock(S().mutex);
  S().qos.erase(session_id);
}

std::vector<Found> Search(int timeout_ms, std::optional<uint64_t> session_id) {
  auto& s = S();
  if (!s.running) return {};
  uint32_t request_id = 0;
  auto pending = NewPending(&request_id);
  Writer w;
  WriteHeader(w, {Type::kSearchRequest, s.config.title_id, s.instance, request_id});
  w.u8(session_id ? 1 : 0);
  w.u64(session_id.value_or(0));
  auto targets = SearchTargets();
  pending->sent_at = NowMs();
  // Send twice: a single lost broadcast should not hide a lobby. Answers are
  // collected by the service thread for the whole timeout.
  for (int round = 0; round < 2; ++round) {
    for (const auto& t : targets) SendTo(t, w);
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms / 2));
  }
  DropPending(request_id);
  std::lock_guard lock(pending->mutex);
  return pending->found;
}

QosResult Qos(uint32_t ip, uint64_t session_id, int timeout_ms) {
  auto& s = S();
  if (!s.running) return {};
  uint16_t port = s.config.remote_port;
  {
    std::lock_guard lock(s.mutex);
    auto it = s.host_ports.find(ip);
    if (it != s.host_ports.end()) port = it->second;
  }
  uint32_t request_id = 0;
  auto pending = NewPending(&request_id);
  pending->qos_session = session_id;
  Writer w;
  WriteHeader(w, {Type::kQosRequest, s.config.title_id, s.instance, request_id});
  w.u64(session_id);
  w.u64(NowMs());
  pending->sent_at = NowMs();
  const int tries = 3;
  for (int i = 0; i < tries; ++i) {
    SendTo({ip, port}, w);
    std::unique_lock lock(pending->mutex);
    if (pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms / tries),
                             [&] { return pending->qos.has_value(); })) {
      break;
    }
  }
  DropPending(request_id);
  std::lock_guard lock(pending->mutex);
  return pending->qos.value_or(QosResult{});
}

}  // namespace harmony::netplay::lobby
