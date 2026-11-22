#pragma once

#import <MetalKit/MetalKit.h>
#include <libretro_vulkan.h>

class RetroVideo {
public:
    bool start(const retro_hw_render_context_negotiation_interface_vulkan& negotiation);
    void stop();
    bool present(MTKView* phone, MTKView* external, bool dual);
    static CGRect fit(CGRect frame, double width, double height);
    void setPictureAspect(float aspect) { pictureAspect = aspect; }
    float topPictureAspect() const { return pictureAspect; }
    CGRect picture(CGRect frame) const {
        const bool topScreen = textureSize.width > textureSize.height * 1.5;
        return topScreen ? fit(frame, pictureAspect, 1)
                         : fit(frame, textureSize.width, textureSize.height);
    }
    void softwareFrame(const void* data, unsigned width, unsigned height, size_t pitch);
    bool ready() const { return interface.device != VK_NULL_HANDLE; }

    retro_hw_render_interface_vulkan interface{};
private:
    float pictureAspect = 400.f / 240.f;
    CGSize textureSize = CGSizeMake(400, 240);
    VkImage image = VK_NULL_HANDLE;
    id<MTLTexture> softwareTexture;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
};
