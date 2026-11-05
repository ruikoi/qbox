// SPDX-License-Identifier: Apache-2.0
#include <systemc>
double sc_time_stamp() { return sc_core::sc_time_stamp().to_seconds() * 1e9; }
bool sim_trace_enabled() { return false; }
