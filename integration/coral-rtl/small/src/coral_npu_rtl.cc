// SPDX-License-Identifier: Apache-2.0
// Experimental platform wrapper for Google's actual Verilated RVV Coral RTL.
#include <cstring>
#include <iostream>
#include <cci_configuration>
#include <module_factory_registery.h>
#include <ports/initiator-signal-socket.h>
#include "tests/verilator_sim/coralnpu/core_mini_axi_tb.h"

class CoralNpuRtl : public sc_core::sc_module {
 public:
  tlm_utils::simple_target_socket<CoralNpuRtl> target_socket{"target_socket"};
  tlm_utils::simple_initiator_socket<CoralNpuRtl> bus_master{"bus_master"};
  InitiatorSignalSocket<bool> irq_out{"irq_out"};
  cci::cci_param<std::string> waveform{"waveform", "", "FST output path"};
  cci::cci_param<unsigned> memory_latency_ns{"memory_latency_ns", 20, "External memory latency"};
  SC_HAS_PROCESS(CoralNpuRtl);
  explicit CoralNpuRtl(sc_core::sc_module_name name)
      : sc_module(name), dut_("rtl", 0, false, false, false, false,
                             std::nullopt, std::nullopt, true) {
    target_socket.register_b_transport(this, &CoralNpuRtl::HostAccess);
    target_socket.register_transport_dbg(this, &CoralNpuRtl::DebugAccess);
    target_socket.register_get_direct_mem_ptr(this, &CoralNpuRtl::NoDmi);
    native_.bind(dut_.host_mmio);
    dut_.external_memory.bind(dma_target_);
    dma_target_.register_b_transport(this, &CoralNpuRtl::DmaAccess);
    Verilated::traceEverOn(true);
    SC_THREAD(Monitor);
    SC_THREAD(Trace);
    SC_METHOD(UpdateIrq);
    sensitive << irq_update_;
  }
  ~CoralNpuRtl() {
    std::cout << "CORAL_RTL_STATS host_transactions=" << host_transactions_
              << " external_axi_transactions=" << dma_transactions_
              << " completions=" << completions_ << " faults=" << faults_ << '\n';
  }
 private:
  CoreMiniAxi_tb dut_;
  tlm_utils::simple_initiator_socket<CoralNpuRtl> native_{"native"};
  tlm_utils::simple_target_socket<CoralNpuRtl> dma_target_{"dma_target"};
  uint32_t status_ = 0, irq_enable_ = 0, irq_pending_ = 0;
  bool running_ = false;
  sc_core::sc_event irq_update_;
  void UpdateIrq() {
    if (irq_out.size()) irq_out->write((irq_pending_ & irq_enable_) != 0);
  }
  uint64_t started_ticks_ = 0, elapsed_ns_ = 0;
  uint64_t host_transactions_ = 0, dma_transactions_ = 0, completions_ = 0, faults_ = 0;
  void Trace() {
    wait(sc_core::SC_ZERO_TIME);
    wait(sc_core::SC_ZERO_TIME);
    if (!waveform.get_value().empty()) dut_.trace_for_platform(dut_.core(), waveform.get_value());
  }
  void Monitor() {
    while (true) {
      wait(dut_.clock.posedge_event());
      if (running_ && (dut_.io_wfi.read() || dut_.io_fault.read())) {
        running_ = false;
        status_ = dut_.io_fault.read() ? 2 : 1;
        irq_pending_ |= status_;
        elapsed_ns_ = static_cast<uint64_t>((sc_core::sc_time_stamp() -
            sc_core::sc_time::from_value(started_ticks_)) / sc_core::sc_time(1, sc_core::SC_NS));
        if (status_ == 1) ++completions_; else ++faults_;
        irq_update_.notify(sc_core::SC_ZERO_TIME);
      }

    }
  }
  void HostAccess(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
    ++host_transactions_;
    const auto addr = trans.get_address();
    if (addr < 0x40000) {
      native_->b_transport(trans, delay);
      if (!trans.is_response_error() && trans.is_write() && addr == 0x30000 &&
          trans.get_data_length() == 4) {
        const uint8_t control = trans.get_data_ptr()[0];
        if (control & 1) { running_ = false; status_ = 0; }
        else if (!(control & 2)) {
          running_ = true; status_ = 4; started_ticks_ = sc_core::sc_time_stamp().value();
        }
      }
      return;
    }
    if (trans.get_data_length() != 4 || trans.get_byte_enable_ptr() ||
        trans.get_streaming_width() < 4 || (addr & 3)) {
      trans.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE); return;
    }
    uint32_t value = 0;
    if (trans.is_write()) {
      for (unsigned i=0;i<4;++i) value |= uint32_t(trans.get_data_ptr()[i]) << (8*i);
      switch (addr) {
        case 0x80008: irq_enable_ = value & 3; break;
        case 0x8000c: irq_pending_ &= ~(value & 3); break;
        default: trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); return;
      }
    } else if (trans.is_read()) {
      switch (addr) {
        case 0x80000: value = 0x4352544c; break; // CRTL
        case 0x80004: value = status_; break;
        case 0x80008: value = irq_enable_; break;
        case 0x8000c: value = irq_pending_; break;
        case 0x80010: value = elapsed_ns_; break;
        case 0x80014: value = elapsed_ns_ >> 32; break;
        case 0x80018: value = dma_transactions_; break;
        default: trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); return;
      }
      for (unsigned i=0;i<4;++i) trans.get_data_ptr()[i] = value >> (8*i);
    } else { trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE); return; }
    irq_update_.notify(sc_core::SC_ZERO_TIME);
    wait(1, sc_core::SC_NS); // Wrapper register latency; settle completion IRQ.
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
  }
  void DmaAccess(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
    ++dma_transactions_;
    delay += sc_core::sc_time(memory_latency_ns.get_value(), sc_core::SC_NS);
    bus_master->b_transport(trans, delay);
  }
  unsigned DebugAccess(tlm::tlm_generic_payload&) { return 0; }
  bool NoDmi(tlm::tlm_generic_payload&, tlm::tlm_dmi&) { return false; }
};
extern "C" void module_register() { GSC_MODULE_REGISTER_C(CoralNpuRtl); }
