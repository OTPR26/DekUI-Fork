// SPDX-License-Identifier: GPL-3.0-or-later
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>
// Real backend device/queue validation, intentionally separate from game boot.
extern "C" int rm_renderer_device_check() {
    nri::DeviceCreationDesc creation{};
    creation.graphicsAPI = nri::GraphicsAPI::VK;
    creation.disableVKRayTracing = true;
    nri::Device* device = nullptr;
    if (nri::nriCreateDevice(creation, device) != nri::Result::SUCCESS) return 0;
    nri::CoreInterface core{};
    auto result = nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core);
    nri::Queue* queue = nullptr;
    if (result == nri::Result::SUCCESS)
        result = core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue);
    const bool ready = result == nri::Result::SUCCESS && queue;
    nri::nriDestroyDevice(device);
    return ready ? 1 : 0;
}
