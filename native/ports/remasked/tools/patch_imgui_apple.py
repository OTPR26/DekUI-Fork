#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Use buffer offsets for ImGui on Apple GPUs without native base-vertex draws."""
from pathlib import Path
import sys
path = Path(sys.argv[1]) / 'backends/imgui_impl_vulkan.cpp'
source = path.read_text()
marker = '// ReMasked Apple: rebase the vertex buffer instead of the draw.'
if marker not in source:
    before = '                vkCmdDrawIndexed(command_buffer, pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset, pcmd->VtxOffset + global_vtx_offset, 0);'
    if source.count(before) != 1:
        raise SystemExit('Pinned ImGui draw context changed')
    source = source.replace(before, '''#if defined(REMASKED_APPLE_GAME)
                // ReMasked Apple: rebase the vertex buffer instead of the draw.
                const VkDeviceSize rebased_offset = (VkDeviceSize)(pcmd->VtxOffset + global_vtx_offset) * sizeof(ImDrawVert);
                vkCmdBindVertexBuffers(command_buffer, 0, 1, &rb->VertexBuffer, &rebased_offset);
                vkCmdDrawIndexed(command_buffer, pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset, 0, 0);
#else
''' + before + '\n#endif')
    path.write_text(source)
