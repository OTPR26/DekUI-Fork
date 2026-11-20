if(NOT DEFINED FFX_SOURCE_DIR)
    message(FATAL_ERROR "FFX_SOURCE_DIR is required")
endif()
set(_backend "${FFX_SOURCE_DIR}/sdk/src/backends/vk/ffx_vk.cpp")
file(READ "${_backend}" _source)
set(_original "${_source}")

# The per-effect bindless pool (used by Brixelizer) is created without
# FREE_DESCRIPTOR_SET_BIT although its sets are freed individually on destroy
# (VUID-vkFreeDescriptorSets-descriptorPool-00312), and its maxSets counts pool
# sizes (at most 3) while up to four sets are allocated: texture SRV/UAV and
# buffer SRV/UAV share the storage-buffer size.
set(_anchor "                descriptorPoolCreateInfo.flags         = 0;
                descriptorPoolCreateInfo.poolSizeCount = poolSizeCount;
                descriptorPoolCreateInfo.pPoolSizes    = poolSizes;
                descriptorPoolCreateInfo.maxSets       = poolSizeCount;")
set(_fixed "                descriptorPoolCreateInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                descriptorPoolCreateInfo.poolSizeCount = poolSizeCount;
                descriptorPoolCreateInfo.pPoolSizes    = poolSizes;
                descriptorPoolCreateInfo.maxSets       = 4;")
string(FIND "${_source}" "${_fixed}" _done)
if(_done EQUAL -1)
    string(FIND "${_source}" "${_anchor}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "FidelityFX bindless descriptor pool contract changed")
    endif()
    string(REPLACE "${_anchor}" "${_fixed}" _source "${_source}")
endif()
if(NOT _source STREQUAL _original)
    file(WRITE "${_backend}" "${_source}")
endif()
