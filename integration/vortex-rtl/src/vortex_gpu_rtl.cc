// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <module_factory_registery.h>
#include "afu_bridge.h"
class VortexGpuRtl : public AfuBridge {
public:
  explicit VortexGpuRtl(sc_core::sc_module_name name)
      : AfuBridge(name, std::getenv("VORTEX_RTL_WAVEFORM") ? std::getenv("VORTEX_RTL_WAVEFORM") : "") {}
};
extern "C" void module_register() { GSC_MODULE_REGISTER_C(VortexGpuRtl); }
