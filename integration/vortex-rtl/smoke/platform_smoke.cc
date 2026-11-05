// SPDX-License-Identifier: Apache-2.0
#include <map>
#include "../src/afu_bridge.h"
class Smoke : public sc_core::sc_module {
public:
  tlm_utils::simple_initiator_socket<Smoke> control{"control"};
  tlm_utils::simple_target_socket<Smoke> memory{"memory"};
  bool passed = false;
  SC_HAS_PROCESS(Smoke);
  Smoke(sc_core::sc_module_name name) : sc_module(name) {
    memory.register_b_transport(this, &Smoke::Memory);
    SC_THREAD(Run);
  }
private:
  std::map<uint64_t, uint8_t> ram_;
  void Memory(tlm::tlm_generic_payload &t, sc_core::sc_time &) {
    auto *be = t.get_byte_enable_ptr();
    for (unsigned i = 0; i < t.get_data_length(); ++i) {
      if (be && be[i % t.get_byte_enable_length()] == TLM_BYTE_DISABLED) { continue; }
      if (t.is_write()) { ram_[t.get_address() + i] = t.get_data_ptr()[i]; }
      else { t.get_data_ptr()[i] = ram_[t.get_address() + i]; }
    }
    t.set_response_status(tlm::TLM_OK_RESPONSE);
  }
  uint32_t Access(uint32_t addr, uint32_t value, bool write) {
    tlm::tlm_generic_payload t;
    t.set_address(addr); t.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
    t.set_data_ptr(reinterpret_cast<uint8_t *>(&value)); t.set_data_length(4); t.set_streaming_width(4);
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    control->b_transport(t, delay);
    if (t.is_response_error()) { SC_REPORT_FATAL("SMOKE", "AXI-Lite failed"); }
    return value;
  }
  void Reject(uint64_t addr, unsigned length, tlm::tlm_response_status expected) {
    uint8_t bytes[8]{};
    tlm::tlm_generic_payload t;
    t.set_address(addr); t.set_command(tlm::TLM_READ_COMMAND);
    t.set_data_ptr(bytes); t.set_data_length(length); t.set_streaming_width(length);
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    control->b_transport(t, delay);
    if (t.get_response_status() != expected) { SC_REPORT_FATAL("SMOKE", "Invalid transaction accepted"); }
  }
  void Run() {
    Reject(0x10001, 4, tlm::TLM_BURST_ERROR_RESPONSE);
    Reject(0x10000, 8, tlm::TLM_BURST_ERROR_RESPONSE);
    Reject(0x1fffc, 4, tlm::TLM_ADDRESS_ERROR_RESPONSE);
    if (Access(0x10000, 0, false) != 0x5652544c) { SC_REPORT_FATAL("SMOKE", "Bad identity"); }
    Access(0x1000, 1, true);
    if (Access(0x1000, 0, false) != 1) { SC_REPORT_FATAL("SMOKE", "CP control readback"); }
    Access(0x1000, 0, true);
    if (Access(0x1000, 0, false) != 0) { SC_REPORT_FATAL("SMOKE", "CP disable readback"); }
    wait(200, sc_core::SC_NS);
    auto stopped = Access(0x10008, 0, false);
    wait(200, sc_core::SC_NS);
    if (Access(0x10008, 0, false) != stopped) { SC_REPORT_FATAL("SMOKE", "Idle clock did not stop"); }
    Access(0x1000, 1, true);
    if (Access(0x10008, 0, false) <= stopped) { SC_REPORT_FATAL("SMOKE", "Idle wake failed"); }
    passed = true;
    std::cout << "VORTEX_RTL_TLM_CONTROL_PASS cp=RTL gpu=RTL\n";
    sc_core::sc_stop();
  }
};
int sc_main(int argc, char **argv) {
  AfuBridge dut("rtl", argc > 1 ? argv[1] : "");
  Smoke smoke("host");
  smoke.control.bind(dut.target_socket);
  dut.bus_master.bind(smoke.memory);
  sc_core::sc_start();
  return smoke.passed ? 0 : 1;
}
