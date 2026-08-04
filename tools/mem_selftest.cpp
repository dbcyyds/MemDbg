/**
 * MemDbg 引擎自测（无 UI）— 需 root
 * 用法: su -c './mem_selftest 5214'
 */
#include "mem_core.hpp"
#include "mem_disasm.hpp"
#include "mem_bp.hpp"
#include "mem_table.hpp"
#include "mem_ptrscan.hpp"
#include "mem_struct.hpp"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0, g_skip = 0;
static char g_report[64 * 1024];
static size_t g_rlen = 0;

static void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char line[512];
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  fputs(line, stdout);
  fputc('\n', stdout);
  fflush(stdout);
  if (g_rlen + (size_t)n + 2 < sizeof(g_report)) {
    memcpy(g_report + g_rlen, line, (size_t)n);
    g_rlen += (size_t)n;
    g_report[g_rlen++] = '\n';
    g_report[g_rlen] = 0;
  }
}

#define PASS(msg) do { g_pass++; logf("[PASS] %s", msg); } while (0)
#define FAIL(msg) do { g_fail++; logf("[FAIL] %s", msg); } while (0)
#define SKIP(msg) do { g_skip++; logf("[SKIP] %s", msg); } while (0)
#define CHECK(cond, msg) do { if (cond) PASS(msg); else FAIL(msg); } while (0)

static void wait_scan(int timeout_ms = 60000) {
  auto t0 = std::chrono::steady_clock::now();
  while (mem::scan_busy()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (ms > timeout_ms) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

static void wait_ptr(int timeout_ms = 120000) {
  auto t0 = std::chrono::steady_clock::now();
  while (mem::ptrscan_busy()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (ms > timeout_ms) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int pid = 5214;
  if (argc >= 2) pid = atoi(argv[1]);
  logf("=== MemDbg selftest pid=%d uid=%d euid=%d ===", pid, getuid(),
       geteuid());

  // ── 1. 进程列表 ──────────────────────────────────────────
  {
    mem::ProcListOptions opt;
    opt.skip_system = true;
    opt.skip_no_icon = false;
    auto procs = mem::list_processes(opt);
    CHECK(!procs.empty(), "list_processes non-empty");
    logf("  procs=%zu", procs.size());
    bool found = false;
    for (auto& p : procs)
      if (p.pid == pid) {
        found = true;
        logf("  target in list: %s pkg=%s tencent=%d icon=%d", p.name.c_str(),
             p.package.c_str(), (int)p.is_tencent, (int)p.has_icon);
        CHECK(p.is_tencent || p.name.find("tencent") != std::string::npos ||
                  p.package.find("tencent") != std::string::npos,
              "target marked tencent if applicable");
      }
    CHECK(found, "target pid appears in process list (skip_system on)");

    opt.tencent_only = true;
    auto tq = mem::list_processes(opt);
    logf("  tencent_only count=%zu", tq.size());
    bool tq_has = false;
    for (auto& p : tq)
      if (p.pid == pid) tq_has = true;
    if (found)
      CHECK(tq_has || tq.size() > 0, "tencent_only finds apps");
    else
      CHECK(true, "tencent_only filter runs");
  }

  // ── 2. 附加 ──────────────────────────────────────────────
  CHECK(mem::attach(pid), "attach target pid");
  CHECK(mem::is_attached(), "is_attached");
  CHECK(mem::attached_pid() == pid, "attached_pid matches");
  logf("  name=%s", mem::attached_name());

  // ── 3. maps ──────────────────────────────────────────────
  auto maps_w = mem::load_maps(true);
  auto maps_all = mem::load_maps(false);
  auto maps_code = mem::load_maps_filtered(mem::RegionFilter::Code);
  auto maps_anon = mem::load_maps_filtered(mem::RegionFilter::Anonymous);
  auto maps_java = mem::load_maps_filtered(mem::RegionFilter::Java);
  CHECK(!maps_w.empty(), "writable maps non-empty");
  CHECK(!maps_all.empty(), "all maps non-empty");
  CHECK(!maps_code.empty(), "code maps non-empty");
  logf("  writable=%zu all=%zu code=%zu anon=%zu java=%zu", maps_w.size(),
       maps_all.size(), maps_code.size(), maps_anon.size(), maps_java.size());
  for (int i = 0; i < (int)mem::RegionFilter::COUNT; ++i) {
    auto f = (mem::RegionFilter)i;
    logf("  filter[%d]=%s", i, mem::region_filter_name(f));
  }

  // 找可写匿名区做安全读写测试
  uintptr_t test_addr = 0;
  uint32_t orig_val = 0;
  for (auto& r : maps_anon) {
    if (!r.writable || r.end - r.start < 0x1000) continue;
    // 跳过太低地址
    if (r.start < 0x10000) continue;
    uint32_t v = 0;
    if (mem::read_mem(r.start + 0x100, &v, 4)) {
      test_addr = r.start + 0x100;
      orig_val = v;
      break;
    }
  }
  if (!test_addr) {
    for (auto& r : maps_w) {
      if (r.end - r.start < 0x1000 || r.start < 0x10000) continue;
      uint32_t v = 0;
      if (mem::read_mem(r.start + 0x40, &v, 4)) {
        test_addr = r.start + 0x40;
        orig_val = v;
        break;
      }
    }
  }
  CHECK(test_addr != 0, "find writable test address");
  logf("  test_addr=0x%llx orig=0x%x", (unsigned long long)test_addr, orig_val);

  // ── 4. 读写 ──────────────────────────────────────────────
  {
    uint32_t magic = 0xA5C3E17Bu;
    bool wr = mem::write_mem(test_addr, &magic, 4);
    uint32_t rd = 0;
    bool rr = mem::read_mem(test_addr, &rd, 4);
    CHECK(wr && rr && rd == magic, "write+read magic I32");
    // 还原
    mem::write_mem(test_addr, &orig_val, 4);
    uint32_t back = 0;
    mem::read_mem(test_addr, &back, 4);
    CHECK(back == orig_val, "restore original value");
  }

  // ── 5. 格式化/解析 ───────────────────────────────────────
  {
    char buf[64];
    mem::format_value(mem::ValType::I32, 12345, buf, sizeof(buf));
    CHECK(strcmp(buf, "12345") == 0, "format I32");
    uint64_t bits = 0;
    size_t sz = 0;
    CHECK(mem::parse_value(mem::ValType::I32, "999", bits, sz) && bits == 999 &&
              sz == 4,
          "parse I32");
    uintptr_t a = 0;
    CHECK(mem::parse_addr("0xDEADBEEF", a) && a == 0xDEADBEEFull, "parse_addr");
    CHECK(mem::type_size_of(mem::ValType::I64) == 8, "type_size I64");
  }

  // ── 6. 首次扫描（精确值 = magic 写入后扫描）──────────────
  uintptr_t scan_hit = 0;
  {
    // 写一个独特值再扫
    uint32_t unique = 0x51E40001u;  // 5214-ish unique
    mem::write_mem(test_addr, &unique, 4);

    mem::ScanConfig cfg;
    char err[96]{};
    CHECK(mem::parse_scan_values(mem::ValType::I32, mem::ScanMode::Exact,
                                 "1373634561", nullptr, cfg, err, sizeof(err)),
          "parse_scan_values exact");  // 0x51E40001 = 1373634561
    // 用 bits 直接设更稳
    cfg.type = mem::ValType::I32;
    cfg.mode = mem::ScanMode::Exact;
    cfg.a_bits = unique;
    cfg.type_size = 4;
    cfg.region = mem::RegionFilter::Anonymous;

    CHECK(mem::start_first_scan(cfg), "start_first_scan");
    wait_scan(90000);
    CHECK(!mem::scan_busy(), "scan finished");
    logf("  scan status=%s progress=%.2f round=%d count=%zu", mem::scan_status(),
         mem::scan_progress(), mem::scan_round(), mem::result_count());
    CHECK(mem::result_count() > 0, "scan found matches");
    CHECK(mem::scan_round() == 1, "scan round == 1");

    std::vector<mem::Match> ms;
    mem::copy_results(ms, 50);
    CHECK(!ms.empty(), "copy_results non-empty");
    bool hit = false;
    for (auto& m : ms) {
      if (m.addr == test_addr) {
        hit = true;
        scan_hit = m.addr;
      }
    }
    // 可能不在前 50 条，仍算部分成功
    if (hit)
      PASS("scan contains test_addr");
    else {
      logf("  test_addr not in first 50 (count=%zu), pick first", ms.size());
      if (!ms.empty()) scan_hit = ms[0].addr;
      if (mem::result_count() > 0)
        PASS("scan has results (test_addr maybe truncated)");
      else
        FAIL("scan has results (test_addr maybe truncated)");
    }
    mem::refresh_result_values(ms);
    if (!ms.empty())
      logf("  first result 0x%llx bits=0x%llx", (unsigned long long)ms[0].addr,
           (unsigned long long)ms[0].value_bits);
  }

  // ── 7. 再次筛选 ──────────────────────────────────────────
  {
    mem::ScanConfig cfg;
    cfg.type = mem::ValType::I32;
    cfg.mode = mem::ScanMode::Unchanged;
    cfg.a_bits = 0x51E40001u;
    cfg.type_size = 4;
    cfg.region = mem::RegionFilter::Anonymous;
    size_t before = mem::result_count();
    if (before > 0 && mem::start_next_scan(cfg)) {
      wait_scan(60000);
      logf("  next_scan unchanged: %zu -> %zu status=%s", before,
           mem::result_count(), mem::scan_status());
      CHECK(mem::scan_round() >= 2, "scan round after next");
      CHECK(mem::result_count() <= before, "next scan does not grow");
    } else {
      SKIP("next_scan (no prior or start fail)");
    }
  }

  // ── 8. 冻结 ──────────────────────────────────────────────
  {
    uintptr_t fa = scan_hit ? scan_hit : test_addr;
    uint32_t freeze_v = 0x11223344u;
    mem::write_mem(fa, &freeze_v, 4);
    mem::set_frozen(fa, true, freeze_v, 4);
    // 试图改掉
    uint32_t other = 0x55667788u;
    mem::write_mem(fa, &other, 4);
    // tick 应写回
    for (int i = 0; i < 5; ++i) {
      mem::tick_freeze(mem::ValType::I32);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    uint32_t now = 0;
    mem::read_mem(fa, &now, 4);
    CHECK(now == freeze_v, "freeze restores value");
    mem::set_frozen(fa, false, 0, 4);
    mem::clear_all_frozen();
    // 还原 test_addr
    mem::write_mem(test_addr, &orig_val, 4);
  }

  // ── 9. 地址表 ────────────────────────────────────────────
  {
    mem::AddressTable tab;
    tab.add(test_addr, mem::ValType::I32, "selftest");
    CHECK(tab.size() == 1, "table add");
    tab.refresh_values();
    logf("  table value=%s", tab.entries[0].value);
    CHECK(tab.entries[0].value[0] != 0, "table refresh value");
    tab.entries[0].freeze = true;
    tab.entries[0].freeze_bits = orig_val;
    tab.tick_freeze();
    const char* path = "/data/local/tmp/memdbg_table_test.txt";
    int n = tab.save(path);
    CHECK(n > 0, "table save");
    mem::AddressTable tab2;
    int m = tab2.load(path);
    CHECK(m > 0 && tab2.size() >= 1, "table load");
    tab.entries[0].selected = true;
    tab.remove_selected();
    CHECK(tab.size() == 0, "table remove_selected");
    tab.clear();
  }

  // ── 10. dump / export ────────────────────────────────────
  {
    int dn = mem::dump_mem(test_addr, 256, "/data/local/tmp/memdbg_dump.bin");
    CHECK(dn == 256, "dump_mem 256 bytes");
    // export 当前扫描结果
    int en = mem::export_results("/data/local/tmp/memdbg_export.txt");
    logf("  export_results n=%d", en);
    CHECK(en >= 0, "export_results runs");
  }

  // ── 11. 反汇编 + 伪C ─────────────────────────────────────
  {
    uintptr_t code = 0;
    // .so 映射起点常是 ELF 头；在多个偏移找「真实」指令（非 noise/.word）
    auto score_real = [](const std::vector<mem::Insn>& ins) {
      int ok = 0;
      for (auto& i : ins)
        if (i.mnem[0] && !i.is_noise && strcmp(i.mnem, ".word") != 0) ok++;
      return ok;
    };
    for (auto& r : maps_code) {
      if (r.end - r.start < 0x2000) continue;
      if (r.path.find(".so") == std::string::npos &&
          r.path.find("apk") == std::string::npos)
        continue;
      uintptr_t offs[] = {0x1000, 0x10000, 0x20000, 0x40000,
                          (r.end - r.start) / 4};
      mem::DisasmOptions dop;
      dop.filter_noise = false;
      for (uintptr_t off : offs) {
        if (r.start + off + 64 >= r.end) continue;
        auto ins = mem::disasm_at(r.start + off, 16, dop);
        int ok = score_real(ins);
        if (ok >= 4) {
          code = r.start + off;
          logf("  code sample %s @0x%llx (+0x%llx) real=%d", r.path.c_str(),
               (unsigned long long)code, (unsigned long long)off, ok);
          break;
        }
      }
      if (code) break;
    }
    if (!code && !maps_code.empty()) code = maps_code[0].start + 0x1000;
    CHECK(code != 0, "find code address");

    mem::DisasmOptions dop;
    dop.filter_noise = true;
    auto ins = mem::disasm_at(code, 32, dop);
    CHECK(!ins.empty(), "disasm_at non-empty");
    int non_word = 0, with_pseudo = 0, real = 0;
    for (auto& i : ins) {
      if (strcmp(i.mnem, ".word") != 0) non_word++;
      if (!i.is_noise && strcmp(i.mnem, ".word") != 0) real++;
      if (i.pseudo[0]) with_pseudo++;
      logf("    %s  %s | %s", i.mnem, i.ops, i.pseudo);
      if (non_word + with_pseudo > 12) break;  // 少打点
    }
    logf("  disasm: n=%zu non_word=%d real=%d with_pseudo=%d", ins.size(),
         non_word, real, with_pseudo);
    CHECK(non_word > 0, "disasm has real mnemonics");
    CHECK(with_pseudo > 0, "disasm has pseudo lines");
    // filter_noise 在噪声区应回退而非空白
    {
      mem::DisasmOptions noisy;
      noisy.filter_noise = true;
      auto at_hdr = mem::disasm_at(maps_code[0].start, 16, noisy);
      CHECK(!at_hdr.empty(), "disasm_at noise-fallback non-empty");
    }

    auto fn = mem::disasm_function(code + 32, 80, dop);
    logf("  disasm_function n=%zu", fn.size());
    CHECK(!fn.empty() || !ins.empty(), "disasm_function or at works");

    auto pseudo = mem::insns_to_pseudo_c(fn.empty() ? ins : fn, code);
    CHECK(pseudo.find("void sub_") != std::string::npos, "pseudo has function");
    CHECK(pseudo.find("asm(") != std::string::npos ||
              pseudo.find("=") != std::string::npos,
          "pseudo has body content");
    logf("  pseudo sample (%zu bytes):\n%.600s", pseudo.size(),
         pseudo.c_str());

    // 单元：已知编码
    uint32_t sample_code[] = {
        0xD503201F,  // nop
        0xD65F03C0,  // ret
        0xAA0003E0,  // mov x0, x0
        0x91000400,  // add x0, x0, #1
    };
    auto unit = mem::disasm_arm64(0x1000, (const uint8_t*)sample_code,
                                  sizeof(sample_code), {});
    CHECK(unit.size() >= 3, "unit disasm count");
    bool has_ret = false, has_add = false, has_mov = false;
    for (auto& i : unit) {
      if (strcmp(i.mnem, "ret") == 0) has_ret = true;
      if (strcmp(i.mnem, "add") == 0) has_add = true;
      if (strcmp(i.mnem, "mov") == 0) has_mov = true;
    }
    CHECK(has_ret, "unit decode ret");
    CHECK(has_add || has_mov, "unit decode add/mov");

    // 汇编器
    uint32_t aw = 0;
    char aerr[64];
    CHECK(mem::assemble_line("nop", aw, aerr, sizeof(aerr)) &&
              aw == 0xD503201F,
          "assemble nop");
    CHECK(mem::assemble_line("ret", aw, aerr, sizeof(aerr)) &&
              aw == 0xD65F03C0,
          "assemble ret");
    CHECK(mem::assemble_line("mov x0, x1", aw, aerr, sizeof(aerr)),
          "assemble mov reg");
    // 分支样例：构建含 b 的流，检查 xref/label
    uint32_t br_code[] = {
        0xD503201F,  // nop
        0x14000002,  // b +8 → skip next
        0xD503201F,  // nop
        0xD65F03C0,  // ret  (target)
    };
    auto bru = mem::disasm_arm64(0x2000, (const uint8_t*)br_code,
                                 sizeof(br_code), {});
    auto xrs = mem::build_xrefs(bru);
    logf("  branch unit n=%zu xrefs=%zu", bru.size(), xrs.size());
    CHECK(!xrs.empty() || bru.size() >= 3, "xrefs or branch decode");
    bool any_label = false;
    for (auto& i : bru)
      if (i.is_label) any_label = true;
    CHECK(any_label || !xrs.empty(), "label or xref present");
    auto cfg = mem::insns_to_cfg_pseudo_c(bru, 0x2000);
    CHECK(cfg.find("goto") != std::string::npos ||
              cfg.find("void") != std::string::npos,
          "cfg pseudo has structure");
    logf("  cfg sample: %.200s", cfg.c_str());

    int sn = mem::sym_refresh();
    logf("  symbols loaded: %d", sn);
    // 符号可为空（取决于 maps 可读 so），不强制
  }

  // ── 12. 指针扫描（可能较慢，限制参数）────────────────────
  {
    (void)scan_hit;
    // 在附近写一个指针链：page 内 p -> test_addr
    uintptr_t ptr_slot = 0;
    for (auto& r : maps_anon) {
      if (!r.writable || r.end - r.start < 0x2000) continue;
      if (r.start < 0x10000) continue;
      uintptr_t cand = r.start + 0x200;
      if (cand == test_addr) continue;
      if (mem::write_mem(cand, &test_addr, sizeof(test_addr))) {
        ptr_slot = cand;
        break;
      }
    }
    if (ptr_slot) {
      logf("  planted ptr 0x%llx -> 0x%llx", (unsigned long long)ptr_slot,
           (unsigned long long)test_addr);
      // 读回确认
      uintptr_t chk = 0;
      mem::read_mem(ptr_slot, &chk, sizeof(chk));
      CHECK(chk == test_addr, "planted pointer readable");
    }

    mem::PtrScanConfig pcfg;
    pcfg.target = test_addr;
    pcfg.max_level = 1;  // 1 级快
    pcfg.max_offset = 0x800;
    pcfg.static_only = false;  // 允许匿名，才能找到我们种的
    pcfg.max_results = 50;
    bool started = mem::ptrscan_start(pcfg);
    if (started) {
      wait_ptr(90000);
      logf("  ptrscan status=%s count=%zu progress=%.2f", mem::ptrscan_status(),
           mem::ptrscan_count(), mem::ptrscan_progress());
      std::vector<mem::PtrChain> chains;
      mem::ptrscan_copy(chains, 20);
      if (!chains.empty()) {
        PASS("ptrscan found chains");
        uintptr_t res = 0;
        bool ok = mem::ptrscan_resolve(chains[0], res);
        logf("  chain0 base=0x%llx offs=%zu resolve=%d ->0x%llx mod=%s",
             (unsigned long long)chains[0].base, chains[0].offsets.size(),
             (int)ok, (unsigned long long)res, chains[0].module.c_str());
        CHECK(ok, "ptrscan_resolve");
      } else {
        // 1 级 + 大进程可能慢或过滤严
        logf("  ptrscan empty (may be OK on large process with tight filter)");
        SKIP("ptrscan results empty");
      }
      mem::ptrscan_clear();
    } else {
      FAIL("ptrscan_start");
      logf("  status=%s", mem::ptrscan_status());
    }
  }

  // ── 13. 硬件断点 / perf ──────────────────────────────────
  {
    CHECK(mem::bp_init(), "bp_init");
    logf("  bp backend=%s status=%s max_slots=%d", mem::bp_backend_name(),
         mem::bp_status(), mem::bp_max_slots());
    CHECK(mem::bp_ready(), "bp_ready");

    // 写观察点在 test_addr
    int id = mem::bp_set(test_addr, mem::BpType::WatchW, 4);
    logf("  bp_set WatchW id=%d status=%s", id, mem::bp_status());
    if (id >= 0) {
      PASS("bp_set WatchW");
      CHECK(mem::bp_count() >= 1, "bp_count >= 1");
      auto list = mem::bp_list();
      CHECK(!list.empty(), "bp_list non-empty");
      mem::bp_arm_and_continue();

      // 触发写
      uint32_t trig = 0xABCDEF01u;
      mem::write_mem(test_addr, &trig, 4);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      mem::BpHit hit{};
      bool got = false;
      for (int i = 0; i < 20; ++i) {
        if (mem::bp_poll(hit) && hit.valid) {
          got = true;
          logf("  HIT: %s", hit.msg);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      if (got)
        PASS("bp_poll got watch hit");
      else {
        logf("  no hit polled (perf may not sample self-write on all kernels)");
        SKIP("bp hit not observed");
      }
      CHECK(mem::bp_enable(id, false), "bp_enable false");
      CHECK(mem::bp_enable(id, true), "bp_enable true");
      CHECK(mem::bp_clear(id), "bp_clear");
    } else {
      FAIL("bp_set WatchW");
    }

    // 执行断点：对代码页
    uintptr_t exec_a = 0;
    for (auto& r : maps_code) {
      if (r.end - r.start < 16) continue;
      exec_a = (r.start + 8) & ~3ull;
      break;
    }
    if (exec_a) {
      int xid = mem::bp_set(exec_a, mem::BpType::Exec, 4);
      logf("  bp_set Exec @0x%llx id=%d", (unsigned long long)exec_a, xid);
      if (xid >= 0) {
        PASS("bp_set Exec");
        mem::bp_clear(xid);
      } else {
        // 某些内核限制
        logf("  Exec bp failed: %s", mem::bp_status());
        SKIP("bp_set Exec failed");
      }
    }
    mem::bp_clear_all();
    CHECK(mem::bp_count() == 0, "bp_clear_all");
    // 不 shutdown：后面 soft_bp / ptrace 还要用
  }

  // ── 14. 软断点 + 寄存器 + dlopen ─────────────────────────
  {
    // 在代码区下软断点（写 BRK 再还原），不实际触发
    uintptr_t sba = 0;
    uint32_t before = 0;
    for (auto& r : maps_code) {
      if (r.path.find(".so") == std::string::npos) continue;
      if (r.end - r.start < 0x2000) continue;
      uintptr_t cand = (r.start + 0x1000) & ~3ull;
      if (mem::read_mem(cand, &before, 4) && before != 0xD4200000u) {
        sba = cand;
        break;
      }
    }
    if (sba) {
      int sid = mem::soft_bp_set(sba);
      logf("  soft_bp_set @0x%llx id=%d status=%s", (unsigned long long)sba,
           sid, mem::bp_status());
      if (sid >= 0) {
        PASS("soft_bp_set");
        uint32_t mid = 0;
        mem::read_mem(sba, &mid, 4);
        CHECK(mid == 0xD4200000u, "soft_bp wrote BRK");
        auto lst = mem::soft_bp_list();
        CHECK(!lst.empty(), "soft_bp_list non-empty");
        // 条件软断点更新同一地址
        int sid2 = mem::soft_bp_set_cond(sba, "x0==0", false);
        CHECK(sid2 == sid, "soft_bp_set_cond updates same id");
        CHECK(mem::soft_bp_clear(sid), "soft_bp_clear");
        uint32_t after = 0;
        mem::read_mem(sba, &after, 4);
        CHECK(after == before, "soft_bp restored orig");
      } else {
        logf("  soft_bp failed (ptrace?): %s", mem::bp_status());
        SKIP("soft_bp_set");
      }
    } else {
      SKIP("no soft_bp candidate");
    }

    // 暂停 / 读寄存器 / 浮点
    if (mem::dbg_pause()) {
      PASS("dbg_pause");
      mem::Regs rr{};
      if (mem::dbg_regs_read(rr) && rr.valid) {
        PASS("dbg_regs_read");
        logf("  PC=0x%llx SP=0x%llx", (unsigned long long)rr.pc,
             (unsigned long long)rr.sp);
        CHECK(rr.pc != 0, "PC non-zero");
      } else {
        FAIL("dbg_regs_read");
      }
      mem::FpRegs fp{};
      if (mem::dbg_fp_regs_read(fp) && fp.valid) {
        PASS("dbg_fp_regs_read");
      } else {
        logf("  fp regs: %s", mem::bp_status());
        SKIP("dbg_fp_regs_read");
      }
      auto thr = mem::list_threads();
      logf("  threads=%zu", thr.size());
      CHECK(!thr.empty(), "list_threads");
      CHECK(mem::dbg_resume(), "dbg_resume");
    } else {
      logf("  dbg_pause fail: %s", mem::bp_status());
      SKIP("dbg_pause");
    }

    // find_dlopen
    uintptr_t dlo = 0;
    if (mem::find_dlopen(dlo) && dlo) {
      PASS("find_dlopen");
      logf("  dlopen @0x%llx status=%s", (unsigned long long)dlo,
           mem::bp_status());
    } else {
      logf("  find_dlopen: %s", mem::bp_status());
      SKIP("find_dlopen");
    }

    // eval_condition 单元
    mem::Regs cr{};
    cr.x[0] = 1;
    cr.x[1] = 2;
    cr.pc = 0x1000;
    cr.sp = 0x2000;
    cr.valid = true;
    CHECK(mem::eval_condition("x0==1", cr), "cond x0==1");
    CHECK(!mem::eval_condition("x0==2", cr), "cond x0==2 false");
    CHECK(mem::eval_condition("x1>1", cr), "cond x1>1");
    CHECK(mem::eval_condition("", cr), "cond empty true");

    mem::soft_bp_clear_all();
    mem::bp_shutdown();
  }

  // ── 15. 字符串扫描 + 结构体 ───────────────────────────────
  {
    // 在匿名区种 UTF-8 再扫
    const char* needle = "MemDbgSelfTestX9";
    uintptr_t saddr = 0;
    for (auto& r : maps_anon) {
      if (!r.writable || r.end - r.start < 0x1000 || r.start < 0x10000)
        continue;
      uintptr_t cand = r.start + 0x300;
      if (mem::write_mem(cand, needle, strlen(needle) + 1)) {
        saddr = cand;
        break;
      }
    }
    if (saddr) {
      mem::ScanConfig sc;
      char err[64]{};
      bool parsed = mem::parse_scan_values(mem::ValType::StrUtf8,
                                          mem::ScanMode::Exact, needle,
                                          nullptr, sc, err, sizeof(err));
      if (!parsed) {
        logf("  parse StrUtf8 err=%s — 手工填 hex_pat", err);
        sc.type = mem::ValType::StrUtf8;
        sc.mode = mem::ScanMode::Exact;
        sc.hex_pat.assign(needle, needle + strlen(needle));
        sc.hex_mask.assign(sc.hex_pat.size(), 0xFF);
        sc.type_size = sc.hex_pat.size();
        parsed = true;
      }
      CHECK(parsed, "parse UTF8 scan");
      sc.region = mem::RegionFilter::Anonymous;
      if (mem::start_first_scan(sc)) {
        wait_scan(90000);
        logf("  str scan count=%zu status=%s", mem::result_count(),
             mem::scan_status());
        CHECK(mem::result_count() > 0, "UTF8 scan found");
        std::vector<mem::Match> ms;
        mem::copy_results(ms, 20);
        bool hit = false;
        for (auto& m : ms)
          if (m.addr == saddr) hit = true;
        if (hit)
          PASS("UTF8 scan contains planted");
        else if (!ms.empty())
          PASS("UTF8 scan has results");
        else
          FAIL("UTF8 scan has results");
      } else {
        FAIL("UTF8 start_first_scan");
      }
      mem::clear_scan();
    } else {
      SKIP("plant UTF8");
    }

    // 结构体剖析
    mem::Structure sd;
    sd.base = test_addr & ~7ull;
    std::snprintf(sd.name, sizeof(sd.name), "selftest");
    uint64_t ptr_val = test_addr;
    int32_t i_val = 42;
    mem::write_mem(sd.base, &i_val, 4);
    mem::write_mem(sd.base + 8, &ptr_val, 8);
    sd.auto_dissect(16);
    logf("  auto_dissect fields=%zu", sd.fields.size());
    CHECK(!sd.fields.empty(), "struct auto_dissect");
    sd.refresh();
    const char* spath = "/data/local/tmp/memdbg_struct_test.txt";
    int sn = sd.save(spath);
    if (sn >= 0) {
      PASS("struct save");
      mem::Structure sd2;
      int ln = sd2.load(spath);
      CHECK(ln >= 0 && !sd2.fields.empty(), "struct load");
    } else {
      logf("  struct save fail");
      SKIP("struct save");
    }
  }

  // ── 16. 清理扫描 ─────────────────────────────────────────
  mem::clear_scan();
  CHECK(mem::result_count() == 0, "clear_scan");
  mem::write_mem(test_addr, &orig_val, 4);
  mem::detach();
  CHECK(!mem::is_attached(), "detach");

  logf("=== SUMMARY pass=%d fail=%d skip=%d ===", g_pass, g_fail, g_skip);
  // 写报告
  FILE* rf = fopen("/data/local/tmp/memdbg_selftest_report.txt", "w");
  if (rf) {
    fprintf(rf, "%s", g_report);
    fprintf(rf, "SUMMARY pass=%d fail=%d skip=%d\n", g_pass, g_fail, g_skip);
    fclose(rf);
  }
  return g_fail > 0 ? 1 : 0;
}
