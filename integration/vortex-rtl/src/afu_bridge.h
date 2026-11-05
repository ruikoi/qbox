// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <cstdlib>
#include <ports/initiator-signal-socket.h>
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vvortex_afu_shim.h"
#include "Vvortex_afu_shim___024root.h"

class AfuBridge : public sc_core::sc_module {
public:
  tlm_utils::simple_target_socket<AfuBridge> target_socket{"target_socket"};
  tlm_utils::simple_initiator_socket<AfuBridge> bus_master{"bus_master"};
  InitiatorSignalSocket<bool> irq_out{"irq_out"};
  uint64_t cycles = 0, controls = 0, host_reads = 0, host_writes = 0;
  uint64_t vram_reads = 0, vram_writes = 0;
  SC_HAS_PROCESS(AfuBridge);
  AfuBridge(sc_core::sc_module_name name, const std::string &wave = "")
      : sc_module(name), dut_(&context_) {
    context_.randReset(0);
    context_.traceEverOn(!wave.empty());
    if (!wave.empty()) {
      trace_ = std::make_unique<VerilatedVcdC>();
      dut_.trace(trace_.get(), 8);
      trace_->open(wave.c_str());
    }
    target_socket.register_b_transport(this, &AfuBridge::Access);
    target_socket.register_transport_dbg(this, &AfuBridge::Debug);
    target_socket.register_get_direct_mem_ptr(this, &AfuBridge::NoDmi);
    BindPorts();
    dut_.ap_rst_n = 0;
    dut_.s_axi_ctrl_awvalid = 0;
    dut_.s_axi_ctrl_wvalid = 0;
    dut_.s_axi_ctrl_arvalid = 0;
    dut_.s_axi_ctrl_bready = 1;
    dut_.s_axi_ctrl_rready = 1;
    if (const char *v = std::getenv("VORTEX_RTL_BACKPRESSURE")) { stalls_ = std::atoi(v) != 0; }
    SC_THREAD(Run);
    SC_METHOD(UpdateIrq);
    sensitive << irq_update_;
  }
  ~AfuBridge() {
    dut_.final();
    if (trace_) {
      trace_->close();
    }
    std::cout << "VORTEX_RTL_STATS cycles=" << cycles << " controls=" << controls
              << " host_reads=" << host_reads << " host_writes=" << host_writes
              << " vram_reads=" << vram_reads << " vram_writes=" << vram_writes
              << " cp=RTL gpu=RTL fpu=DPI\n";
  }
private:
  static constexpr unsigned kBytes = 64, kBanks = 2;
  static constexpr uint64_t kVram = 0x100000000ull;
  struct ReadBeat {
    std::array<uint8_t, kBytes> data{};
    uint32_t id;
    bool last;
    sc_core::sc_time ready;
  };
  struct Address {
    uint64_t addr;
    uint32_t id, length, beat = 0;
  };
  struct WriteBeat {
    std::array<uint8_t, kBytes> data{};
    uint64_t strobe;
    bool last;
  };
  struct Port {
    CData *awvalid, *awready, *awlen, *wvalid, *wready, *wlast;
    CData *arvalid, *arready, *arlen, *rvalid, *rready, *rlast, *rresp;
    CData *bvalid, *bready, *bresp;
    QData *awaddr, *araddr, *wstrb;
    IData *awid, *arid, *rid, *bid;
    VlWide<16> *wdata, *rdata;
    std::deque<Address> addresses;
    std::deque<WriteBeat> writes;
    std::deque<ReadBeat> reads;
    std::deque<uint32_t> replies;
  };
  VerilatedContext context_;
  Vvortex_afu_shim dut_;
  std::unique_ptr<VerilatedVcdC> trace_;
  std::array<Port, kBanks + 1> ports_{};
  sc_core::sc_mutex control_lock_;
  sc_core::sc_event wake_, ready_, completion_;
  bool initialized_ = false, clock_enabled_ = false, control_pending_ = false;
  uint32_t control_value_ = 0;
  uint32_t irq_pending_ = 0, irq_enable_ = 0, irq_pulses_ = 0;
  uint64_t stalled_aw_ = 0, stalled_w_ = 0, stalled_ar_ = 0, gated_ = 0;
  bool stalls_ = false, irq_level_ = false;
  sc_core::sc_event irq_update_;
  void UpdateIrq() {
    if (irq_out.size()) { irq_out->write((irq_pending_ & irq_enable_) != 0); }
  }
  unsigned control_cycles_ = 0, idle_cycles_ = 0;
  bool Idle() const {
    const auto *r = dut_.rootp;
    if (control_pending_ || !r->vortex_afu_shim__DOT__afu_wrap__DOT__mem_idle_all ||
        !r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__fetch_idle[0] ||
        !r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__engine_idle[0] ||
        r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__u_completion__DOT__pending[0] ||
        !r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__u_completion__DOT__fifo_empty ||
        r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__u_completion__DOT__state) { return false; }
    for (const auto &p : ports_) {
      if (!p.addresses.empty() || !p.writes.empty() || !p.reads.empty() || !p.replies.empty()) { return false; }
    }
    return true;
  }

#define BIND_PORT(index, prefix) \
  { auto &p = ports_[index]; \
    p.awvalid=&dut_.prefix##_awvalid; p.awready=&dut_.prefix##_awready; \
    p.awaddr=&dut_.prefix##_awaddr; p.awid=&dut_.prefix##_awid; p.awlen=&dut_.prefix##_awlen; \
    p.wvalid=&dut_.prefix##_wvalid; p.wready=&dut_.prefix##_wready; \
    p.wdata=&dut_.prefix##_wdata; p.wstrb=&dut_.prefix##_wstrb; p.wlast=&dut_.prefix##_wlast; \
    p.arvalid=&dut_.prefix##_arvalid; p.arready=&dut_.prefix##_arready; \
    p.araddr=&dut_.prefix##_araddr; p.arid=&dut_.prefix##_arid; p.arlen=&dut_.prefix##_arlen; \
    p.rvalid=&dut_.prefix##_rvalid; p.rready=&dut_.prefix##_rready; \
    p.rdata=&dut_.prefix##_rdata; p.rid=&dut_.prefix##_rid; p.rlast=&dut_.prefix##_rlast; p.rresp=&dut_.prefix##_rresp; \
    p.bvalid=&dut_.prefix##_bvalid; p.bready=&dut_.prefix##_bready; \
    p.bid=&dut_.prefix##_bid; p.bresp=&dut_.prefix##_bresp; }
  void BindPorts() {
    BIND_PORT(0, m_axi_mem_0);
    BIND_PORT(1, m_axi_mem_1);
    BIND_PORT(2, m_axi_host);
    for (auto &p : ports_) {
      *p.awready = *p.wready = *p.arready = 1;
      *p.rvalid = *p.bvalid = 0;
      *p.rresp = *p.bresp = 0;
      *p.rid = *p.bid = 0;
      *p.rlast = 0;
      std::memset(p.rdata->data(), 0, kBytes);
    }
  }
#undef BIND_PORT

  sc_core::sc_time Memory(unsigned bank, uint64_t addr, uint8_t *data,
                          uint64_t strobe, bool write) {
    if (bank < kBanks) {
      // VX_mem_to_axi restores the full byte address on each bank port.
      addr += kVram;
      if (write) { ++vram_writes; } else { ++vram_reads; }
    } else {
      if (addr < 0x20000000 || addr > 0x24000000 - kBytes) {
        SC_REPORT_FATAL("VORTEX_AXI", "Host DMA outside staging RAM");
      }
      if (write) { ++host_writes; } else { ++host_reads; }
    }
    tlm::tlm_generic_payload trans;
    std::array<uint8_t, kBytes> enables{};
    for (unsigned i = 0; i < kBytes; ++i) {
      enables[i] = (strobe >> i) & 1 ? TLM_BYTE_ENABLED : TLM_BYTE_DISABLED;
    }
    trans.set_address(addr);
    trans.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
    trans.set_data_ptr(data);
    trans.set_data_length(kBytes);
    trans.set_streaming_width(kBytes);
    if (write) {
      trans.set_byte_enable_ptr(enables.data());
      trans.set_byte_enable_length(kBytes);
    }
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    bus_master->b_transport(trans, delay);
    if (trans.is_response_error()) {
      std::cerr << "VORTEX_AXI address=0x" << std::hex << addr << std::dec << '\n';
      SC_REPORT_FATAL("VORTEX_AXI", trans.get_response_string().c_str());
    }
    return delay;
  }

  void SamplePort(unsigned index) {
    auto &p = ports_[index];
    if (*p.rvalid && *p.rready) { p.reads.pop_front(); }
    if (*p.bvalid && *p.bready) { p.replies.pop_front(); }
    if (*p.arvalid && *p.arready) {
      for (unsigned i = 0; i <= *p.arlen; ++i) {
        ReadBeat beat{};
        beat.id = *p.arid;
        beat.last = i == *p.arlen;
        auto delay = Memory(index, *p.araddr + i * kBytes, beat.data.data(), ~0ull, false);
        if (stalls_) { delay += sc_core::sc_time(11 + index * 3, sc_core::SC_NS); }
        beat.ready = sc_core::sc_time_stamp() + delay;
        p.reads.push_back(beat);
      }
    }
    if (*p.awvalid && *p.awready) {
      p.addresses.push_back({*p.awaddr, *p.awid, unsigned(*p.awlen) + 1});
    }
    if (*p.wvalid && *p.wready) {
      WriteBeat beat{};
      std::memcpy(beat.data.data(), p.wdata->data(), kBytes);
      beat.strobe = *p.wstrb;
      beat.last = *p.wlast;
      p.writes.push_back(beat);
    }
    if (!p.addresses.empty() && !p.writes.empty()) {
      auto &addr = p.addresses.front();
      auto &beat = p.writes.front();
      if (beat.last != (addr.beat + 1 == addr.length)) {
        SC_REPORT_FATAL("VORTEX_AXI", "WLAST disagrees with AWLEN");
      }
      Memory(index, addr.addr + addr.beat * kBytes, beat.data.data(), beat.strobe, true);
      ++addr.beat;
      p.writes.pop_front();
      if (addr.beat == addr.length) {
        p.replies.push_back(addr.id);
        p.addresses.pop_front();
      }
    }
  }
  void DrivePort(Port &p) {
    *p.awready = p.addresses.size() < 16 && (!stalls_ || cycles % 7 >= 3);
    *p.wready = p.writes.size() < 512 && (!stalls_ || cycles % 11 >= 5);
    *p.arready = p.reads.size() < 512 && (!stalls_ || cycles % 13 >= 6);
    if (*p.awvalid && !*p.awready) { ++stalled_aw_; }
    if (*p.wvalid && !*p.wready) { ++stalled_w_; }
    if (*p.arvalid && !*p.arready) { ++stalled_ar_; }
    *p.rvalid = !p.reads.empty() && p.reads.front().ready <= sc_core::sc_time_stamp();
    if (*p.rvalid) {
      auto &beat = p.reads.front();
      *p.rid = beat.id;
      *p.rlast = beat.last;
      std::memcpy(p.rdata->data(), beat.data.data(), kBytes);
    }
    *p.bvalid = !p.replies.empty();
    if (*p.bvalid) { *p.bid = p.replies.front(); }
  }
  void Edge() {
    context_.time(cycles * 2);
    dut_.ap_clk = 0;
    dut_.eval();
    if (trace_ && cycles < 20000) { trace_->dump(cycles * 2); }
    bool aw = dut_.s_axi_ctrl_awvalid && dut_.s_axi_ctrl_awready;
    bool w = dut_.s_axi_ctrl_wvalid && dut_.s_axi_ctrl_wready;
    bool ar = dut_.s_axi_ctrl_arvalid && dut_.s_axi_ctrl_arready;
    bool done = control_pending_ && (dut_.s_axi_ctrl_bvalid || dut_.s_axi_ctrl_rvalid);
    if (done && ((dut_.s_axi_ctrl_bvalid && dut_.s_axi_ctrl_bresp) ||
                 (dut_.s_axi_ctrl_rvalid && dut_.s_axi_ctrl_rresp))) {
      SC_REPORT_FATAL("VORTEX_AXIL", "RTL rejected a control transaction");
    }
    if (dut_.s_axi_ctrl_rvalid) { control_value_ = dut_.s_axi_ctrl_rdata; }
    if (dut_.ap_rst_n) {
      for (unsigned i = 0; i < ports_.size(); ++i) { SamplePort(i); }
    }
    context_.time(cycles * 2 + 1);
    dut_.ap_clk = 1;
    dut_.eval();
    if (trace_ && cycles < 20000) { trace_->dump(cycles * 2 + 1); }
    if (dut_.__SYM__interrupt && dut_.ap_rst_n) {
      irq_pending_ |= 1;
      if (!irq_level_) { ++irq_pulses_; }
      irq_update_.notify(sc_core::SC_ZERO_TIME);
    }
    irq_level_ = dut_.__SYM__interrupt != 0;
    if (aw) { dut_.s_axi_ctrl_awvalid = 0; }
    if (w) { dut_.s_axi_ctrl_wvalid = 0; }
    if (ar) { dut_.s_axi_ctrl_arvalid = 0; }
    for (auto &p : ports_) { DrivePort(p); }
    ++cycles;
    if (done) {
      control_pending_ = false;
      completion_.notify(sc_core::SC_ZERO_TIME);
    }
    if (control_pending_ && ++control_cycles_ > 100000) {
      SC_REPORT_FATAL("VORTEX_AXIL", "Control transaction timeout");
    }
    if (initialized_) {
      idle_cycles_ = Idle() ? idle_cycles_ + 1 : 0;
      if (idle_cycles_ >= 64) { clock_enabled_ = false; ++gated_; }
    }
    if (cycles % 10000000 == 0) {
      const auto *r = dut_.rootp;
      std::cout << "VORTEX_RTL_PROGRESS cycles=" << cycles << " controls=" << controls
                << " idle=" << Idle() << " host_reads=" << host_reads << " host_writes=" << host_writes
                << " vram_reads=" << vram_reads << " vram_writes=" << vram_writes
                << " cp_busy=" << unsigned(r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__cp_busy)
                << " fetch_idle=" << unsigned(r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__fetch_idle[0])
                << " engine_idle=" << unsigned(r->vortex_afu_shim__DOT__afu_wrap__DOT__cp_core__DOT__engine_idle[0]) << std::endl;
    }
    if (context_.gotFinish()) { SC_REPORT_FATAL("VORTEX_RTL", "RTL terminated"); }
  }
  void Run() {
    for (unsigned i = 0; i < 64; ++i) { Edge(); wait(1, sc_core::SC_NS); }
    dut_.ap_rst_n = 1;
    for (unsigned i = 0; i < 64; ++i) { Edge(); wait(1, sc_core::SC_NS); }
    initialized_ = true;
    ready_.notify(sc_core::SC_ZERO_TIME);
    while (true) {
      if (!clock_enabled_) { wait(wake_); }
      Edge();
      wait(1, sc_core::SC_NS);
    }
  }
  void Access(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
    if (trans.get_data_length() != 4 || trans.get_byte_enable_ptr() ||
        trans.get_streaming_width() < 4 || (trans.get_address() & 3)) {
      trans.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
      return;
    }
    if (!trans.is_read() && !trans.is_write()) {
      trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
      return;
    }
    if (delay != sc_core::SC_ZERO_TIME) { wait(delay); delay = sc_core::SC_ZERO_TIME; }
    control_lock_.lock();
    if (!initialized_) { wait(ready_); }
    uint32_t value = 0;
    const auto addr = trans.get_address();
    if (trans.is_write()) { std::memcpy(&value, trans.get_data_ptr(), 4); }
    if (addr >= 0x10000) {
      if (trans.is_read()) {
        switch (addr) {
        case 0x10000: value = 0x5652544c; break;
        case 0x10004: value = 1; break;
        case 0x10008: value = cycles; break;
        case 0x1000c: value = cycles >> 32; break;
        case 0x10010: value = host_reads; break;
        case 0x10014: value = host_writes; break;
        case 0x10018: value = vram_reads; break;
        case 0x1001c: value = vram_writes; break;
        case 0x10024: value = irq_enable_; break;
        case 0x10028: value = irq_pending_; break;
        case 0x1002c: value = irq_pulses_; break;
        case 0x10030: value = stalled_aw_; break;
        case 0x10034: value = stalled_w_; break;
        case 0x10038: value = stalled_ar_; break;
        case 0x1003c: value = gated_; break;
        default: trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); control_lock_.unlock(); return;
        }
      } else if (addr == 0x10024) {
        irq_enable_ = value & 1;
        irq_update_.notify(sc_core::SC_ZERO_TIME);
      } else if (addr == 0x10028) {
        irq_pending_ &= ~(value & 1);
        irq_update_.notify(sc_core::SC_ZERO_TIME);
      } else if (addr == 0x10020) {
        clock_enabled_ = value != 0;
      } else {
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        control_lock_.unlock();
        return;
      }
    } else {
      clock_enabled_ = true;
      control_pending_ = true;
      control_cycles_ = 0;
      idle_cycles_ = 0;
      if (trans.is_write()) {
        dut_.s_axi_ctrl_awaddr = addr;
        dut_.s_axi_ctrl_awvalid = 1;
        dut_.s_axi_ctrl_wdata = value;
        dut_.s_axi_ctrl_wstrb = 15;
        dut_.s_axi_ctrl_wvalid = 1;
      } else {
        dut_.s_axi_ctrl_araddr = addr;
        dut_.s_axi_ctrl_arvalid = 1;
      }
      ++controls;
      wake_.notify(sc_core::SC_ZERO_TIME);
      wait(completion_);
      value = control_value_;
    }
    if (trans.is_read()) { std::memcpy(trans.get_data_ptr(), &value, 4); }
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    control_lock_.unlock();
  }
  unsigned Debug(tlm::tlm_generic_payload &) { return 0; }
  bool NoDmi(tlm::tlm_generic_payload &, tlm::tlm_dmi &) { return false; }
};
