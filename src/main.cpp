// SPDX-License-Identifier: MIT
// cpp-irq-watch — Linux IRQ distribution monitor (/proc/interrupts)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o irq-watch
// Usage: ./irq-watch [--watch N] [--top N] [--json] [--no-color] [--selftest]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Mutable ANSI palette (--no-color must actually clear it — see skill pitfall:
// constexpr/static-init palettes make --no-color a no-op)
// ─────────────────────────────────────────────────────────────────────────────
static std::string C_RESET  = "\x1b[0m";
static std::string C_BOLD   = "\x1b[1m";
static std::string C_DIM    = "\x1b[2m";
static std::string C_RED    = "\x1b[31m";
static std::string C_GREEN  = "\x1b[32m";
static std::string C_YELLOW = "\x1b[33m";
static std::string C_BLUE   = "\x1b[34m";
static std::string C_CYAN   = "\x1b[36m";

static void disable_color() {
  C_RESET = C_BOLD = C_DIM = C_RED = C_GREEN = C_YELLOW = C_BLUE = C_CYAN = "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Data model
// ─────────────────────────────────────────────────────────────────────────────
struct IrqEntry {
  int irq = -1;                       // -1 for named entries (NMI, LOC, ERR, ...)
  std::string id;                     // raw identifier as it appears ("0", "LOC")
  std::string name;                   // device/description (continuations merged)
  std::vector<unsigned long long> counts;  // per-CPU counts (padded to num_cpus)
  unsigned long long total() const {
    unsigned long long s = 0;
    for (auto c : counts) s += c;
    return s;
  }
};

struct Snapshot {
  int num_cpus = 0;
  std::vector<IrqEntry> irqs;
  unsigned long long total = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Parsing
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<unsigned long long> parse_ull(std::string_view s) {
  // std::from_chars does NOT skip leading whitespace — trim first
  auto start = s.find_first_not_of(" \t");
  if (start == std::string_view::npos) return std::nullopt;
  s.remove_prefix(start);
  unsigned long long v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc()) return std::nullopt;
  if (ptr != s.data() + s.size()) return std::nullopt;  // trailing garbage
  return v;
}

static std::vector<std::string> split_ws(std::string_view s) {
  std::vector<std::string> out;
  std::istringstream ss{std::string(s)};
  std::string tok;
  while (ss >> tok) out.push_back(tok);
  return out;
}

// Parse the full text of /proc/interrupts (pure — used by selftest with
// synthetic data). Handles: header line, per-IRQ rows, named rows (NMI, LOC,
// ...), single-count rows (ERR/MIS) and multi-line continuations.
static std::optional<Snapshot> parse_interrupts(const std::string& text) {
  Snapshot snap;
  std::istringstream in{text};
  std::string line;

  // Header: "            CPU0       CPU1       CPU2       CPU3"
  // (skip blank lines until the header line is found)
  bool found_header = false;
  while (std::getline(in, line)) {
    auto toks = split_ws(line);
    if (toks.empty()) continue;
    snap.num_cpus = 0;
    for (auto& t : toks) {
      if (t.rfind("CPU", 0) == 0) ++snap.num_cpus;
    }
    if (snap.num_cpus == 0) snap.num_cpus = static_cast<int>(toks.size());
    found_header = true;
    break;
  }
  if (!found_header) return std::nullopt;

  IrqEntry* last = nullptr;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \t") == std::string::npos) continue;  // blank
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      // Continuation line (IRQ rows wrap when there are many CPUs): it starts
      // indented and carries extra per-CPU counts and/or the rest of the name.
      if (!last) continue;
      auto toks = split_ws(line);
      for (auto& t : toks) {
        if (static_cast<int>(last->counts.size()) < snap.num_cpus) {
          if (auto v = parse_ull(t)) {
            last->counts.push_back(*v);
            continue;
          }
        }
        if (!last->name.empty()) last->name += ' ';
        last->name += t;
      }
      continue;
    }

    std::string id_part = line.substr(0, colon);
    // trim trailing whitespace of the id part
    while (!id_part.empty() && (id_part.back() == ' ' || id_part.back() == '\t'))
      id_part.pop_back();
    if (id_part.empty()) continue;

    std::string_view rest(line.data() + colon + 1, line.size() - colon - 1);
    auto toks = split_ws(rest);

    // A continuation line looks like "       <spaces>text..." — it has no
    // "id:" prefix, so it never reaches here (colon == npos → skipped above).
    // But a row whose id is only digits vs named both parse here.
    IrqEntry e;
    e.id = id_part;
    if (auto v = parse_ull(id_part)) {
      e.irq = static_cast<int>(*v);
    } else {
      e.irq = -1;  // named entry (NMI, LOC, ERR, ...)
    }

    // Numeric tokens first = counts; the rest = device name.
    size_t i = 0;
    for (; i < toks.size(); ++i) {
      auto v = parse_ull(toks[i]);
      if (!v) break;
      e.counts.push_back(*v);
    }
    for (; i < toks.size(); ++i) {
      if (!e.name.empty()) e.name += ' ';
      e.name += toks[i];
    }
    // Pad to num_cpus (ERR/MIS have a single total count)
    while (static_cast<int>(e.counts.size()) < snap.num_cpus) e.counts.push_back(0);
    if (e.name.empty()) e.name = e.id;

    snap.irqs.push_back(std::move(e));
    last = &snap.irqs.back();
  }

  for (auto& e : snap.irqs) snap.total += e.total();
  return snap;
}

static std::optional<Snapshot> read_proc_interrupts() {
  std::ifstream f("/proc/interrupts");
  if (!f) return std::nullopt;
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return parse_interrupts(text);
}

// Demo mode: parse the embedded synthetic sample (useful on containers/VMs
// where /proc/interrupts is empty or masked)
static std::optional<Snapshot> demo_snapshot() {
  // Increment counters per call (LCG) so watch mode shows non-zero rates
  static unsigned long long s_seed = 0x9E3779B97F4A7C15ULL;
  static unsigned long long s_call = 0;
  const auto jitter = [&]() -> unsigned long long {
    s_seed = s_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (s_seed >> 33) % 50000ULL;  // 0..49999 int/s per frame
  };
  static const std::string demo = R"(            CPU0       CPU1       CPU2       CPU3
  0:    212870536  127398136  123456789   98765432  IR-IO-APIC   2-edge      timer
  1:          123        456        789        321  IR-IO-APIC   1-edge      i8042
  8:            1          2          3          4  IR-IO-APIC   8-edge      rtc0
 24:    123456789  987654321  234567890  123456789  PCI-MSI 327680-edge      xhci_hcd
 25:      1234567    7654321    2345678    8765432  PCI-MSI 327681-edge      nvme0q0
 27:     98765432  123456789  87654321  987654321  PCI-MSI 524289-edge      eth0
 29:      1111111    2222222    3333333    4444444  PCI-MSI 524291-edge      iwlwifi
130:     76543210  876543210  987654321  123456789  PCI-MSI 327682-edge      nvme0q1
NMI:          0          0          0          0   Non-maskable interrupts
LOC: 12345678901 9876543210 1234567890 9876543210   Local timer interrupts
SPU:          0          0          0          0   Spurious interrupts
PMI:          0          0          0          0   Performance monitoring interrupts
IWI:     1234567    2345678    3456789    4567890   IRQ work interrupts
RES:    12345678   23456789   34567890   45678901   Rescheduling interrupts
CAL:        123        456        789        321   Function call interrupts
TLB:      123456     234567     345678     456789   TLB shootdowns
TRM:          0          0          0          0   Thermal event interrupts
THR:          0          0          0          0   Threshold APIC interrupts
DFR:          0          0          0          0   Deferred Error APIC interrupts
MCE:          0          0          0          0   Machine check exceptions
MCP:          0          0          0          0   Machine check polls
ERR:          0
MIS:          0
)";
  auto snap = parse_interrupts(demo);
  if (snap) {
    ++s_call;
    for (auto& e : snap->irqs) {
      for (auto& c : e.counts) c += jitter();
    }
  }
  return snap;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rate computation (watch mode)
// ─────────────────────────────────────────────────────────────────────────────
struct Rates {
  std::vector<std::vector<double>> per_irq_cpu;  // rates per irq per cpu
  std::vector<double> per_irq_total;
  std::vector<double> per_cpu_total;
  double total_rate = 0.0;
};

static Rates compute_rates(const Snapshot& cur, const Snapshot& prev, double dt) {
  Rates r;
  size_t n = cur.irqs.size();
  r.per_irq_cpu.resize(n);
  r.per_irq_total.resize(n, 0.0);
  r.per_cpu_total.resize(static_cast<size_t>(cur.num_cpus), 0.0);
  if (dt <= 0.0) dt = 1.0;

  for (size_t i = 0; i < n; ++i) {
    const auto& e = cur.irqs[i];
    const auto* pe = (i < prev.irqs.size() && prev.irqs[i].id == e.id) ? &prev.irqs[i] : nullptr;
    auto& row = r.per_irq_cpu[i];
    row.resize(e.counts.size(), 0.0);
    for (size_t c = 0; c < e.counts.size(); ++c) {
      unsigned long long base = (pe && c < pe->counts.size()) ? pe->counts[c] : 0;
      double rate = (e.counts[c] > base) ? static_cast<double>(e.counts[c] - base) / dt : 0.0;
      row[c] = rate;
      r.per_irq_total[i] += rate;
      if (c < r.per_cpu_total.size()) r.per_cpu_total[c] += rate;
    }
  }
  for (auto v : r.per_irq_total) r.total_rate += v;
  return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Formatting helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string fmt_count(unsigned long long v) {
  char buf[32];
  if (v >= 1'000'000'000ULL)
    std::snprintf(buf, sizeof(buf), "%.1fG", v / 1e9);
  else if (v >= 1'000'000ULL)
    std::snprintf(buf, sizeof(buf), "%.1fM", v / 1e6);
  else if (v >= 1'000ULL)
    std::snprintf(buf, sizeof(buf), "%.1fk", v / 1e3);
  else
    std::snprintf(buf, sizeof(buf), "%llu", v);
  return buf;
}

static std::string fmt_rate(double v) {
  char buf[32];
  if (v >= 1e9) std::snprintf(buf, sizeof(buf), "%6.1fG", v / 1e9);
  else if (v >= 1e6) std::snprintf(buf, sizeof(buf), "%6.1fM", v / 1e6);
  else if (v >= 1e3) std::snprintf(buf, sizeof(buf), "%6.1fk", v / 1e3);
  else std::snprintf(buf, sizeof(buf), "%7.1f", v);
  return buf;
}

// Semaphore color for a rate (int/s)
static std::string rate_color(double rate) {
  if (rate <= 0.0) return C_DIM;
  if (rate < 500.0) return C_GREEN;
  if (rate < 5000.0) return C_YELLOW;
  return C_RED;
}

static std::string rate_color_total(unsigned long long total, int /*num_cpus*/) {
  if (total == 0) return C_DIM;
  if (total < 10'000'000ULL) return C_GREEN;
  if (total < 1'000'000'000ULL) return C_YELLOW;
  return C_RED;
}

// ─────────────────────────────────────────────────────────────────────────────
// Text rendering
// ─────────────────────────────────────────────────────────────────────────────
struct Options {
  int watch = 0;
  int top = 10;
  bool json = false;
  bool no_color = false;
  bool selftest = false;
  bool demo = false;
};

static void render_text(const Snapshot& snap, const Rates* rates, int top_n) {
  std::println("{}", C_BOLD + "IRQ WATCH — Linux interrupt monitor (/proc/interrupts)" + C_RESET);
  std::println("{}", C_DIM + "  " + std::string(77, '-') + C_RESET);

  // Sort by total (rate if live, raw count otherwise), take top N
  std::vector<size_t> order(snap.irqs.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    double ta = rates ? rates->per_irq_total[a] : static_cast<double>(snap.irqs[a].total());
    double tb = rates ? rates->per_irq_total[b] : static_cast<double>(snap.irqs[b].total());
    if (ta != tb) return ta > tb;
    return snap.irqs[a].id < snap.irqs[b].id;
  });
  int shown = std::min(top_n, static_cast<int>(order.size()));

  std::println("{}", C_CYAN + std::format("  TOP INTERRUPTS ({} of {})", shown, static_cast<int>(order.size())) + C_RESET);

  // Header row
  std::string hdr = "   IRQ     NAME";
  for (int c = 0; c < snap.num_cpus; ++c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "  CPU%-2d", c);
    hdr += "  ";
    hdr += buf;
  }
  hdr += "   TOTAL";
  std::println("{}", C_DIM + hdr + C_RESET);

  int busiest_cpu = 0;
  double busiest_rate = -1.0;
  for (int c = 0; c < snap.num_cpus; ++c) {
    double r = rates ? rates->per_cpu_total[static_cast<size_t>(c)]
                     : 0.0;
    if (r > busiest_rate) { busiest_rate = r; busiest_cpu = c; }
  }

  for (int k = 0; k < shown; ++k) {
    const auto& e = snap.irqs[order[static_cast<size_t>(k)]];
    std::string line;
    char buf[32];
    if (e.irq >= 0) std::snprintf(buf, sizeof(buf), " %4d  ", e.irq);
    else std::snprintf(buf, sizeof(buf), "  %-4s ", e.id.c_str());
    line = "  " + std::string(buf);

    std::string name = e.name;
    if (name.size() > 18) name = name.substr(0, 18);
    while (name.size() < 18) name += ' ';
    line += name;

    double irq_total = rates ? rates->per_irq_total[order[static_cast<size_t>(k)]]
                             : static_cast<double>(e.total());
    std::string col = rate_color(irq_total);
    if (!rates && irq_total > 0) col = rate_color_total(e.total(), snap.num_cpus);

    for (size_t c = 0; c < e.counts.size() && c < static_cast<size_t>(snap.num_cpus); ++c) {
      std::string cell;
      if (rates) {
        cell = fmt_rate(rates->per_irq_cpu[order[static_cast<size_t>(k)]][c]);
      } else {
        cell = fmt_count(e.counts[c]);
        if (cell.size() < 7) cell = std::string(7 - cell.size(), ' ') + cell;
      }
      line += "  " + cell;
    }
    std::string tot = rates ? fmt_rate(irq_total) : fmt_count(e.total());
    if (tot.size() < 7) tot = std::string(7 - tot.size(), ' ') + tot;
    line += "  " + tot;

    std::println("{}", col + line + C_RESET);
  }

  std::println("");
  std::string sum = C_CYAN + "  SUMMARY" + C_RESET + "  irqs " + std::to_string(snap.irqs.size()) +
                    "   cpus " + std::to_string(snap.num_cpus);
  if (rates) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "   total %.1fk int/s", rates->total_rate / 1e3);
    sum += buf;
  } else {
    sum += "   total " + fmt_count(snap.total);
  }
  sum += C_DIM + "   busiest CPU " + std::to_string(busiest_cpu) + C_RESET;
  std::println("{}", sum);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON rendering (keys ALWAYS quoted — skill pitfall)
// ─────────────────────────────────────────────────────────────────────────────
static std::string json_escape(std::string_view s) {
  std::string out;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
    else if (ch == '\n') out += "\\n";
    else out += ch;
  }
  return out;
}

static void render_json(const Snapshot& snap, const Rates* rates) {
  std::println("{{");
  std::println("  \"cpus\": {},", snap.num_cpus);
  std::println("  \"irqs\": [");
  bool first = true;
  for (size_t idx = 0; idx < snap.irqs.size(); ++idx) {
    const auto& e = snap.irqs[idx];
    if (!first) std::println(",");
    first = false;
    std::string line = "    {\"irq\": ";
    if (e.irq >= 0) line += std::to_string(e.irq);
    else line += "\"" + json_escape(e.id) + "\"";
    line += ", \"name\": \"" + json_escape(e.name) + "\"";
    line += ", \"counts\": [";
    for (size_t c = 0; c < e.counts.size(); ++c) {
      if (c) line += ", ";
      line += std::to_string(e.counts[c]);
    }
    line += "]";
    if (rates) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), ", \"rate\": %.1f", rates->per_irq_total[idx]);
      line += buf;
    } else {
      line += ", \"total\": " + std::to_string(e.total());
    }
    line += "}";
    std::cout << line;
  }
  std::println("");
  std::println("  ],");
  std::string sum = "  \"summary\": {\"irqs\": " + std::to_string(snap.irqs.size()) +
                    ", \"cpus\": " + std::to_string(snap.num_cpus) +
                    ", \"total\": \"" + fmt_count(snap.total) + "\"";
  if (rates) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", \"total_rate\": %.1f", rates->total_rate);
    sum += buf;
  }
  sum += "}";  // close summary object (root closes in final println)
  std::println("{}", sum);
  std::println("}}");
}

// ─────────────────────────────────────────────────────────────────────────────
// Selftest
// ─────────────────────────────────────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, std::string_view what) {
  if (ok) {
    ++g_pass;
    std::println("{}  {}", C_GREEN + "  PASS" + C_RESET, what);
  } else {
    ++g_fail;
    std::println("{}  {}", C_RED + "  FAIL" + C_RESET, what);
  }
}

static const char* kSample = R"(
            CPU0       CPU1       CPU2       CPU3
  0:    212870536  127398136  123456789   98765432  IR-IO-APIC   2-edge      timer
  1:          123        456        789        321  IR-IO-APIC   1-edge      i8042
  8:            1          2          3          4  IR-IO-APIC   8-edge      rtc0
 24:    123456789  987654321  234567890  123456789  PCI-MSI 327680-edge      xhci_hcd
 25:      1234567    7654321    2345678    8765432  PCI-MSI 327681-edge      nvme0q0
NMI:          0          0          0          0   Non-maskable interrupts
LOC: 12345678901 9876543210 1234567890 9876543210   Local timer interrupts
ERR:          0
MIS:          0
)";

static int run_selftest() {
  std::println("{}", C_BOLD + "IRQ WATCH selftest" + C_RESET);

  auto snap = parse_interrupts(kSample);
  check(snap.has_value(), "parse synthetic /proc/interrupts");
  if (!snap) return 1;
  check(snap->num_cpus == 4, "header -> 4 CPUs");
  check(snap->irqs.size() == 9, "9 rows parsed (5 IRQ + 4 named)");

  auto timer = std::find_if(snap->irqs.begin(), snap->irqs.end(),
                             [](const IrqEntry& e) { return e.irq == 0; });
  check(timer != snap->irqs.end(), "IRQ 0 present");
  if (timer != snap->irqs.end()) {
    check(timer->counts.size() == 4, "timer has 4 per-CPU counts");
    check(timer->counts[0] == 212870536ULL, "timer CPU0 count");
    check(timer->name.find("timer") != std::string::npos, "timer name");
  }

  auto loc = std::find_if(snap->irqs.begin(), snap->irqs.end(),
                           [](const IrqEntry& e) { return e.id == "LOC"; });
  check(loc != snap->irqs.end() && loc->irq == -1, "named entry LOC (irq=-1)");
  if (loc != snap->irqs.end()) check(loc->counts.size() == 4, "LOC padded to 4 counts");

  auto err = std::find_if(snap->irqs.begin(), snap->irqs.end(),
                           [](const IrqEntry& e) { return e.id == "ERR"; });
  check(err != snap->irqs.end() && err->counts.size() == 4 && err->counts[0] == 0 && err->counts[3] == 0,
        "ERR single count padded with zeros");

  check(parse_ull("  42") == 42ULL, "from_chars trims leading whitespace");
  check(!parse_ull("12abc").has_value(), "from_chars rejects trailing garbage");

  // continuation line merging
  {
    std::string txt = "            CPU0       CPU1\n  0:         100        200  IR-IO-APIC 2-edge\n                timer\n";
    auto s2 = parse_interrupts(txt);
    check(s2.has_value() && s2->irqs.size() == 1, "continuation line handled");
    if (s2) {
      check(s2->irqs[0].name == "IR-IO-APIC 2-edge timer", "continuation merged into name");
    }
  }

  // delta rates
  {
    Snapshot a, b;
    a.num_cpus = b.num_cpus = 2;
    IrqEntry x;
    x.irq = 0; x.id = "0"; x.name = "timer"; x.counts = {100, 100};
    a.irqs.push_back(x);
    IrqEntry y = x;
    y.counts = {200, 150};
    b.irqs.push_back(y);
    auto r = compute_rates(b, a, 1.0);
    check(std::abs(r.per_irq_total[0] - 150.0) < 1e-9, "delta rate = 150/s");
    check(std::abs(r.per_cpu_total[0] - 100.0) < 1e-9 && std::abs(r.per_cpu_total[1] - 50.0) < 1e-9,
          "per-CPU rates correct");
    check(std::abs(r.total_rate - 150.0) < 1e-9, "total rate correct");
  }

  check(fmt_count(48700) == "48.7k", "fmt_count 48700 -> 48.7k");
  check(fmt_count(1234567890ULL) == "1.2G", "fmt_count 1234567890 -> 1.2G");

  // smoke test against real /proc/interrupts (may be restricted in containers)
  auto real = read_proc_interrupts();
  if (real) {
    check(real->num_cpus >= 1 && !real->irqs.empty(), "smoke: real /proc/interrupts parsed");
  } else {
    std::println("{}", C_DIM + "  SKIP  smoke: /proc/interrupts not readable here" + C_RESET);
  }

  std::println("");
  std::println("{}", C_BOLD + std::format("{} passed, {} failed", g_pass, g_fail) + C_RESET);
  return g_fail == 0 ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
static int run_once(const Options& opt, const Rates* rates) {
  auto snap = opt.demo ? demo_snapshot() : read_proc_interrupts();
  if (!snap) {
    std::println("{}", C_RED + "error: cannot read /proc/interrupts" + C_RESET);
    return 1;
  }
  if (opt.json) {
    render_json(*snap, rates);
  } else {
    render_text(*snap, rates, opt.top);
  }
  return 0;
}

int main(int argc, char** argv) {
  Options opt;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto next_val = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::println("{}", C_RED + std::format("error: {} requires a value", flag) + C_RESET);
        std::exit(2);
      }
      return std::string{argv[++i]};
    };
    if (a == "--watch" || a == "-w") {
      auto v = next_val("--watch");
      if (!v) return 2;
      opt.watch = std::atoi(v->c_str());
    } else if (a == "--top" || a == "-t") {
      auto v = next_val("--top");
      if (!v) return 2;
      opt.top = std::atoi(v->c_str());
    } else if (a == "--json") {
      opt.json = true;
    } else if (a == "--no-color" || a == "-c") {
      opt.no_color = true;
      disable_color();
    } else if (a == "--selftest") {
      opt.selftest = true;
    } else if (a == "--demo") {
      opt.demo = true;
    } else if (a == "--help" || a == "-h") {
      std::println("Usage: irq-watch [options]");
      std::println("  --watch N    live refresh every N seconds (rates per second)");
      std::println("  --top N      show top N interrupts (default 10)");
      std::println("  --json       full JSON output for scripting");
      std::println("  --no-color   disable ANSI colors (pipes/logs)");
      std::println("  --demo       use embedded sample data (containers/VMs)");
      std::println("  --selftest   run self-verification tests");
      return 0;
    } else {
      std::println("{}", C_RED + std::format("error: unknown option {}", a) + C_RESET);
      return 2;
    }
  }

  if (opt.selftest) return run_selftest();
  if (opt.top <= 0) opt.top = 10;

  if (opt.watch > 0) {
    Snapshot prev;
    bool have_prev = false;
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
      auto t1 = std::chrono::steady_clock::now();
      double dt = std::chrono::duration<double>(t1 - t0).count();
      auto snap = opt.demo ? demo_snapshot() : read_proc_interrupts();
      if (!snap) {
        std::println("{}", C_RED + "error: cannot read /proc/interrupts" + C_RESET);
        return 1;
      }
      if (opt.json) {
        if (have_prev) {
          auto rates = compute_rates(*snap, prev, dt);
          render_json(*snap, &rates);
        } else {
          render_json(*snap, nullptr);
        }
      } else {
        if (!opt.no_color) std::println("\x1b[2J\x1b[H");  // clear screen (respect --no-color)
        if (have_prev) {
          auto rates = compute_rates(*snap, prev, dt);
          render_text(*snap, &rates, opt.top);
        } else {
          render_text(*snap, nullptr, opt.top);
        }
        std::println("{}", C_DIM + std::format("  (live refresh every {}s — Ctrl+C to exit)", opt.watch) + C_RESET);
      }
      std::cout << std::flush;  // fully-buffered in pipes — flush per iteration
      prev = std::move(*snap);
      have_prev = true;
      t0 = t1;
      std::this_thread::sleep_for(std::chrono::seconds(opt.watch));
    }
  }

  return run_once(opt, nullptr);
}
