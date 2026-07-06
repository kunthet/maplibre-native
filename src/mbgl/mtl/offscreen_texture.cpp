#include <mbgl/mtl/offscreen_texture.hpp>
#include <mbgl/mtl/context.hpp>
#include <mbgl/mtl/renderable_resource.hpp>
#include <mbgl/mtl/renderer_backend.hpp>
#include <mbgl/mtl/texture2d.hpp>

#include <Metal/Metal.hpp>

#include <cstring>

namespace mbgl {

namespace mtl {

class OffscreenTextureResource final : public RenderableResource {
public:
    OffscreenTextureResource(Context& context_,
                             const Size size_,
                             const gfx::TextureChannelDataType type_,
                             bool depth,
                             [[maybe_unused]] bool stencil)
        : context(context_),
          size(size_),
          type(type_) {
        assert(!size.isEmpty());
        colorTexture = context.createTexture2D();
        colorTexture->setSize(size);
        colorTexture->setFormat(gfx::TexturePixelType::RGBA, type);
        colorTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                               .wrapU = gfx::TextureWrapType::Clamp,
                                               .wrapV = gfx::TextureWrapType::Clamp});
        static_cast<Texture2D*>(colorTexture.get())
            ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);

        if (depth) {
            depthTexture = context.createTexture2D();
            depthTexture->setSize(size);
            depthTexture->setFormat(gfx::TexturePixelType::Depth, gfx::TextureChannelDataType::Float);
            depthTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                   .wrapU = gfx::TextureWrapType::Clamp,
                                                   .wrapV = gfx::TextureWrapType::Clamp});
            static_cast<Texture2D*>(depthTexture.get())
                ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        }

        // On iOS simulator, the depth target is PixelFormatDepth32Float_Stencil8
#if !TARGET_OS_SIMULATOR
        if (stencil) {
            stencilTexture = context.createTexture2D();
            stencilTexture->setSize(size);
            stencilTexture->setFormat(gfx::TexturePixelType::Stencil, gfx::TextureChannelDataType::UnsignedByte);
            stencilTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                     .wrapU = gfx::TextureWrapType::Clamp,
                                                     .wrapV = gfx::TextureWrapType::Clamp});
            static_cast<Texture2D*>(stencilTexture.get())
                ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        }
#endif

        context.renderingStats().numFrameBuffers++;
    }

    ~OffscreenTextureResource() noexcept override { context.renderingStats().numFrameBuffers--; }

    void bind() override {
        assert(context.getBackend().getCommandQueue());
        // A single rendered frame may create several render passes that target
        // this same offscreen renderable (upload pass, 3D pass, the main pass,
        // intermediate render targets, ...). Each RenderPass constructor calls
        // bind(), but only the *last* command buffer is committed later by
        // swap()/present(). Previously the earlier command buffers were simply
        // overwritten here and released WITHOUT being committed.
        //
        // MTLCommandQueue limits the number of *uncommitted* command buffers
        // that may be alive at once (maxCommandBufferCount, 64 by default). On
        // some drivers (e.g. AMD on Intel Macs) an uncommitted-then-released
        // command buffer keeps holding its in-flight slot, so after ~64 frames
        // of interaction [MTLCommandQueue commandBuffer] blocks forever waiting
        // for a free slot — observed as the whole app freezing while panning or
        // zooming the headless Metal map. Commit any leftover command buffer
        // first so its slot is released (committing, not GPU completion, is what
        // frees the uncommitted-buffer slot, so no waitUntilCompleted is needed).
        if (commandBuffer) {
            commandBuffer->commit();
            commandBuffer.reset();
        }
        commandBuffer = NS::RetainPtr(context.getBackend().getCommandQueue()->commandBuffer());
        colorTexture->create();


        renderPassDescriptor = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        if (auto* colorTarget = renderPassDescriptor->colorAttachments()->object(0)) {
            colorTarget->setTexture(static_cast<Texture2D*>(colorTexture.get())->getMetalTexture());
        }

        if (depthTexture) {
            depthTexture->create();
            if (auto* depthTarget = renderPassDescriptor->depthAttachment()) {
                depthTarget->setTexture(static_cast<Texture2D*>(depthTexture.get())->getMetalTexture());
            }
        }
        if (stencilTexture) {
            stencilTexture->create();
            if (auto* stencilTarget = renderPassDescriptor->stencilAttachment()) {
                stencilTarget->setTexture(static_cast<Texture2D*>(stencilTexture.get())->getMetalTexture());
            }
        }
    }

    void swap() override { finishFrame(); }

    void commitFrame() override {
        if (!commandBuffer) {
            return;
        }
        commandBuffer->commit();
        commandBuffer.reset();
        renderPassDescriptor.reset();
    }

    void finishFrame() {
        if (!commandBuffer) {
            return;
        }
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
        commandBuffer.reset();
        renderPassDescriptor.reset();
    }

    PremultipliedImage readStillImage() {
        colorTexture->create();
        auto* texture = static_cast<Texture2D*>(colorTexture.get());
        if (!texture->hasMetalTexture()) {
            return PremultipliedImage(size);
        }

        auto* mtlTexture = texture->getMetalTexture();
        const NS::UInteger bytesPerRow = size.width * colorTexture->getPixelStride();
        const NS::UInteger dataSize = colorTexture->getDataSize();

        // The offscreen color render target may be created with a storage mode
        // that is NOT directly CPU-readable (Managed on Intel/AMD, or a
        // non-synchronized mode on Apple Silicon). Calling getBytes() on such a
        // texture returns stale/zero data (this manifested as fully transparent
        // frames on macOS). Blit the texture into a Shared staging buffer and
        // read from there so the readback works on every device.
        auto& device = context.getBackend().getDevice();
        auto& queue = context.getBackend().getCommandQueue();
        if (device && queue) {

            if (auto stagingBuffer = NS::TransferPtr(
                    device->newBuffer(dataSize, MTL::ResourceStorageModeShared))) {
                auto cmd = NS::RetainPtr(queue->commandBuffer());
                if (auto* blit = cmd->blitCommandEncoder()) {
                    blit->copyFromTexture(mtlTexture,
                                          /*sourceSlice=*/0,
                                          /*sourceLevel=*/0,
                                          MTL::Origin::Make(0, 0, 0),
                                          MTL::Size::Make(size.width, size.height, 1),
                                          stagingBuffer.get(),
                                          /*destinationOffset=*/0,
                                          /*destinationBytesPerRow=*/bytesPerRow,
                                          /*destinationBytesPerImage=*/dataSize);
                    blit->endEncoding();
                    cmd->commit();
                    cmd->waitUntilCompleted();

                    auto data = std::make_unique<uint8_t[]>(dataSize);
                    if (const void* contents = stagingBuffer->contents()) {
                        std::memcpy(data.get(), contents, dataSize);
                        return {size, std::move(data)};
                    }
                }
            }
        }

        // Fallback: direct texture read (works only for Shared-storage textures).
        auto data = std::make_unique<uint8_t[]>(dataSize);
        MTL::Region region = MTL::Region::Make2D(0, 0, size.width, size.height);
        mtlTexture->getBytes(data.get(), bytesPerRow, region, 0);
        return {size, std::move(data)};
    }


    gfx::Texture2DPtr& getTexture() {
        assert(colorTexture);
        return colorTexture;
    }

    const RendererBackend& getBackend() const override { return context.getBackend(); }

    const MTLCommandBufferPtr& getCommandBuffer() const override { return commandBuffer; }

    MTLBlitPassDescriptorPtr getUploadPassDescriptor() const override {
        return NS::TransferPtr(MTL::BlitPassDescriptor::alloc()->init());
    }

    const MTLRenderPassDescriptorPtr& getRenderPassDescriptor() const override {
        assert(renderPassDescriptor);
        return renderPassDescriptor;
    }

private:
    Context& context;
    const Size size;
    const gfx::TextureChannelDataType type;
    gfx::Texture2DPtr colorTexture;
    gfx::Texture2DPtr depthTexture;
    gfx::Texture2DPtr stencilTexture;
    MTLCommandBufferPtr commandBuffer;
    MTLRenderPassDescriptorPtr renderPassDescriptor;
};

OffscreenTexture::OffscreenTexture(
    Context& context, const Size size_, const gfx::TextureChannelDataType type, bool depth, bool stencil)
    : gfx::OffscreenTexture(size, std::make_unique<OffscreenTextureResource>(context, size_, type, depth, stencil)) {}

bool OffscreenTexture::isRenderable() {
    assert(false);
    return true;
}

PremultipliedImage OffscreenTexture::readStillImage() {
    return getResource<OffscreenTextureResource>().readStillImage();
}

const gfx::Texture2DPtr& OffscreenTexture::getTexture() {
    return getResource<OffscreenTextureResource>().getTexture();
}

} // namespace mtl
} // namespace mbgl
