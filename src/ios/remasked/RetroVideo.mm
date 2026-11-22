#import "RetroVideo.h"
#import <MoltenVK/mvk_deprecated_api.h>
#import <QuartzCore/CAMetalLayer.h>
#include <vector>
#include <algorithm>
#include <cstring>

bool RetroVideo::start(const retro_hw_render_context_negotiation_interface_vulkan& negotiation) {
    if (!negotiation.create_device || !negotiation.get_application_info) return false;
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data());
    auto has = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const auto& extension) {
            return strcmp(extension.extensionName, name) == 0;
        });
    };
    std::vector<const char*> extensions;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has(VK_KHR_SURFACE_EXTENSION_NAME)) extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    if (has(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    info.pApplicationInfo = negotiation.get_application_info();
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (vkCreateInstance(&info, nullptr, &interface.instance) != VK_SUCCESS) return false;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(interface.instance, &count, nullptr);
    if (count == 0) { stop(); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(interface.instance, &count, devices.data()) != VK_SUCCESS) {
        stop(); return false;
    }
    retro_vulkan_context context{};
    if (!negotiation.create_device(&context, interface.instance, devices.front(), VK_NULL_HANDLE,
                                   vkGetInstanceProcAddr, nullptr, 0, nullptr, 0, nullptr)) {
        stop(); return false;
    }
    interface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    interface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    interface.handle = this;
    interface.gpu = context.gpu;
    interface.device = context.device;
    interface.queue = context.queue;
    interface.queue_index = context.queue_family_index;
    interface.get_device_proc_addr = vkGetDeviceProcAddr;
    interface.get_instance_proc_addr = vkGetInstanceProcAddr;
    interface.set_image = [](void* host, const retro_vulkan_image* frame, uint32_t count,
                             const VkSemaphore* semaphores, uint32_t) {
        auto& self = *static_cast<RetroVideo*>(host);
        if (count) {
            std::vector<VkPipelineStageFlags> stages(count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = count;
            submit.pWaitSemaphores = semaphores;
            submit.pWaitDstStageMask = stages.data();
            vkQueueSubmit(self.interface.queue, 1, &submit, VK_NULL_HANDLE);
        }
        self.image = frame ? frame->create_info.image : VK_NULL_HANDLE;
    };
    interface.get_sync_index = [](void*) -> uint32_t { return 0; };
    interface.get_sync_index_mask = [](void*) -> uint32_t { return 1; };
    interface.wait_sync_index = [](void* host) {
        vkQueueWaitIdle(static_cast<RetroVideo*>(host)->interface.queue);
    };
    interface.lock_queue = [](void*) {};
    interface.unlock_queue = [](void*) {};
    interface.set_command_buffers = [](void* host, uint32_t count, const VkCommandBuffer* buffers) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = count;
        submit.pCommandBuffers = buffers;
        vkQueueSubmit(static_cast<RetroVideo*>(host)->interface.queue, 1, &submit, VK_NULL_HANDLE);
    };
    return true;
}

void RetroVideo::stop() {
    if (interface.device) {
        vkDeviceWaitIdle(interface.device);
        vkDestroyDevice(interface.device, nullptr);
    }
    if (interface.instance) vkDestroyInstance(interface.instance, nullptr);
    interface = {};
    image = VK_NULL_HANDLE;
    softwareTexture = nil;
    textureSize = CGSizeMake(400, 240);
}

void RetroVideo::softwareFrame(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (!data || width == 0 || height == 0) return;
    if (softwareTexture.width != width || softwareTexture.height != height) {
        auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                  width:width height:height mipmapped:NO];
        softwareTexture = [MTLCreateSystemDefaultDevice() newTextureWithDescriptor:descriptor];
    }
    [softwareTexture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0 withBytes:data bytesPerRow:pitch];
}

CGRect RetroVideo::fit(CGRect frame, double width, double height) {
    const double scale = std::min(frame.size.width / width, frame.size.height / height);
    const double contentWidth = width * scale, contentHeight = height * scale;
    return CGRectMake(CGRectGetMidX(frame) - contentWidth * .5,
                      CGRectGetMidY(frame) - contentHeight * .5, contentWidth, contentHeight);
}

bool RetroVideo::present(MTKView* phone, MTKView* external, bool dual) {
    id<MTLTexture> texture = softwareTexture;
    if (image != VK_NULL_HANDLE) {
        vkQueueWaitIdle(interface.queue);
        id<MTLTexture> native = nil;
        vkGetMTLTextureMVK(image, &native);
        texture = native;
    }
    if (!texture) return false;
    const CGSize nextSize = CGSizeMake(texture.width, texture.height);
    const bool sizeChanged = !CGSizeEqualToSize(nextSize, textureSize);
    textureSize = nextSize;
    // Azahar's default layout uses a 400×480 framebuffer, with the
    // 320×240 bottom display centered below the 400×240 top display.
    const bool stacked = texture.width * 6 == texture.height * 5;
    if (dual != stacked) return sizeChanged;
    if (!pipeline) {
        NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vmain(uint i [[vertex_id]]) { float2 uv=float2((i<<1)&2,i&2);"
            "return V{float4(2*uv.x-1,1-2*uv.y,0,1),uv}; }\n"
            "fragment float4 fmain(V in [[stage_in]],texture2d<float> t [[texture(0)]],"
            "constant float4& crop [[buffer(0)]]) { constexpr sampler s(filter::linear);"
            "return float4(t.sample(s,crop.xy+crop.zw*in.uv).rgb,1); }";
        NSError* error = nil;
        auto library = [phone.device newLibraryWithSource:source options:nil error:&error];
        if (!library) { NSLog(@"ReMasked video shader: %@", error); return sizeChanged; }
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"vmain"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"fmain"];
        descriptor.colorAttachments[0].pixelFormat = phone.colorPixelFormat;
        pipeline = [phone.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) { NSLog(@"ReMasked video pipeline: %@", error); return sizeChanged; }
        queue = [phone.device newCommandQueue];
    }
    auto commands = [queue commandBuffer];
    const auto draw = [&](MTKView* view, bool bottom, bool externalDisplay) {
        id<CAMetalDrawable> drawable;
        MTLRenderPassDescriptor* pass;
        if (externalDisplay) {
            // The external view is driven by the phone's render loop and never
            // enters MTKView's draw cycle, which normally releases its drawable.
            CAMetalLayer* layer = (CAMetalLayer*)view.layer;
            drawable = [layer nextDrawable];
            if (!drawable) return;
            pass = [MTLRenderPassDescriptor renderPassDescriptor];
            auto color = pass.colorAttachments[0];
            color.texture = drawable.texture;
            color.loadAction = MTLLoadActionClear;
            color.storeAction = MTLStoreActionStore;
            color.clearColor = view.clearColor;
        } else {
            pass = view.currentRenderPassDescriptor;
            drawable = view.currentDrawable;
        }
        if (!pass || !drawable) return;
        const CGRect frame = CGRectMake(0, 0, drawable.texture.width, drawable.texture.height);
        const CGRect bounds = bottom ? fit(frame, 320, 240)
            : dual ? fit(frame, pictureAspect, 1) : picture(frame);
        const simd_float4 crop = bottom ? simd_make_float4(.1f, .5f, .8f, .5f)
            : dual ? simd_make_float4(0.f, 0.f, 1.f, .5f) : simd_make_float4(0.f, 0.f, 1.f, 1.f);
        auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setViewport:(MTLViewport){bounds.origin.x, bounds.origin.y,
                                          bounds.size.width, bounds.size.height, 0, 1}];
        [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder setFragmentBytes:&crop length:sizeof(crop) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [commands presentDrawable:drawable];
    };
    if (external) draw(external, false, true);
    if (!external || dual) draw(phone, dual, false);
    [commands commit];
    // The libretro core can overwrite the same Vulkan image on the next tick.
    [commands waitUntilCompleted];
    return sizeChanged;
}
