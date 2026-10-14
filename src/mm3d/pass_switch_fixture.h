// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Vulkan {
class Instance;
class Scheduler;
}
namespace MM3D {
void RunPassSwitchFixture(const Vulkan::Instance&, Vulkan::Scheduler&);
}
