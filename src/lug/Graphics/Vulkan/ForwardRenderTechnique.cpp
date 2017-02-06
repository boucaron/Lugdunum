#include <lug/Config.hpp>
#include <lug/Graphics/DirectionalLight.hpp>
#include <lug/Graphics/MeshInstance.hpp>
#include <lug/Graphics/RenderQueue.hpp>
#include <lug/Graphics/SceneNode.hpp>
#include <lug/Graphics/Spotlight.hpp>
#include <lug/Graphics/Vulkan/ForwardRenderTechnique.hpp>
#include <lug/Graphics/Vulkan/Mesh.hpp>
#include <lug/Graphics/Vulkan/RenderView.hpp>
#include <lug/Math/Matrix.hpp>
#include <lug/Math/Vector.hpp>
#include <lug/Math/Geometry/Transform.hpp>
#include <lug/System/Logger.hpp>

namespace lug {
namespace Graphics {
namespace Vulkan {

using MeshInstance = ::lug::Graphics::MeshInstance;

ForwardRenderTechnique::ForwardRenderTechnique(const RenderView* renderView, const Device* device, Queue* presentQueue) :
                                                RenderTechnique(renderView, device, presentQueue) {}

bool ForwardRenderTechnique::render(const RenderQueue& renderQueue, const Semaphore& imageReadySemaphore,
                                    const Semaphore& drawCompleteSemaphore, uint32_t currentImageIndex) {
    static Math::Mat4x4f projectionMatrix{Math::Mat4x4f::identity()};
    static Math::Mat4x4f viewMatrix{Math::Geometry::lookAt<float>({0.0f, -4.0f, -5.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f})};

    auto& viewport = _renderView->getViewport();

    // Update the projection matrix and rotate model matrix
    {
        projectionMatrix = Math::Geometry::perspective(
            Math::Geometry::radians(_renderView->getCamera()->getFov()),
            viewport.getRatio(),
            0.1f, 100.0f);
    }

    if (!_cmdBuffers[0].begin()) return false;

    // Init render pass
    {
        VkViewport vkViewport{
            vkViewport.x = viewport.offset.x,
            vkViewport.y = viewport.offset.y,
            vkViewport.width = viewport.extent.width,
            vkViewport.height = viewport.extent.height,
            vkViewport.minDepth = viewport.minDepth,
            vkViewport.maxDepth = viewport.maxDepth,
        };

        VkRect2D scissor{};
        scissor.offset = {(int32_t)_renderView->getScissor().offset.x, (int32_t)_renderView->getScissor().offset.y};
        scissor.extent = {(uint32_t)_renderView->getScissor().extent.width, (uint32_t)_renderView->getScissor().extent.height};

        vkCmdSetViewport(_cmdBuffers[0], 0, 1, &vkViewport);
        vkCmdSetScissor(_cmdBuffers[0], 0, 1, &scissor);
    }

    // Update lights buffer data and descriptor set bindings
    {
        for (uint32_t i = 0; i < renderQueue.getLightsNb(); ++i) {
            Light* light = renderQueue.getLights()[i];
            auto& lightBuffer = _lightsBuffers[i];
            uint32_t lightDataSize = 0;
            void* lightData = light->getData(lightDataSize);

            _lightsDescriptorSets[i].update(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER , 0, lightBuffer.get(), 0, lightDataSize);
            lightBuffer->updateDataTransfer(&_cmdBuffers[0], lightData, lightDataSize);
        }
    }

    // Update camera buffer data
    {
        Math::Mat4x4f cameraData[] = {
            viewMatrix,
            projectionMatrix
        };
        _cameraBuffer->updateDataTransfer(&_cmdBuffers[0], cameraData, sizeof(cameraData));
    }


    // Render objects
    {
        VkDeviceSize vertexBufferOffset = 0;
        VkDeviceSize indexBufferOffset = 0;
        // ALl the lights pipelines have the same renderPass
        RenderPass* renderPass = _pipelines[Light::Type::DirectionalLight]->getRenderPass();

        renderPass->begin(
            &_cmdBuffers[0],
            _framebuffers[currentImageIndex],
            {viewport.extent.width, viewport.extent.height},
            {viewport.offset.x, viewport.offset.y}
        );

        // Blend constants are used as dst blend factor
        // We set them to 0 so that there is no blending
        {
            const float blendConstants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            vkCmdSetBlendConstants(_cmdBuffers[0], blendConstants);
        }


        for (std::size_t i = 0; i < renderQueue.getLightsNb(); ++i) {

            {
                if (i == 1) {
                    // Blend constants are used as dst blend factor
                    // Now the depth buffer is filled, we can set the blend constants to 1 to enable blending
                    const float blendConstants[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    vkCmdSetBlendConstants(_cmdBuffers[0], blendConstants);
                }
            }

            auto lightType = renderQueue.getLights()[i]->getLightType();
            auto& lightPipeline = _pipelines[lightType];
            // TODO: WARNING: Find alternative to light type
            auto& descriptorSet = _lightsDescriptorSets[i];

            lightPipeline->bind(&_cmdBuffers[0]);

            // WARNING: it will only works if the pipelines has got the same descriptor set layout
            _cameraDescriptorSet.bind(lightPipeline->getLayout(), &_cmdBuffers[0], 0);

            descriptorSet.bind(lightPipeline->getLayout(), &_cmdBuffers[0], 1);

            for (std::size_t j = 0; j < renderQueue.getObjectsNb(); ++j) {
                auto& object = renderQueue.getObjects()[j];
                if (object->getType() == MovableObject::Type::MESH) {
                    const MeshInstance* meshInstance = static_cast<const MeshInstance*>(object);
                    const Mesh* mesh = static_cast<const Mesh*>(meshInstance->getMesh());
                    const VkBuffer vertexBuffer = *mesh->getVertexBuffer();

                    LUG_ASSERT(object->getParent() != nullptr, "A MovableObject should have a parent");
                    Math::Mat4x4f pushConstants[] = {
                        object->getParent()->getTransform()
                    };
                    vkCmdPushConstants(_cmdBuffers[0], *lightPipeline->getLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants), pushConstants);

                    vkCmdBindVertexBuffers(_cmdBuffers[0], 0, 1, &vertexBuffer, &vertexBufferOffset);
                    vkCmdBindIndexBuffer(_cmdBuffers[0], *mesh->getIndexBuffer(), indexBufferOffset, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(_cmdBuffers[0], (uint32_t)mesh->indices.size(), 1, 0, 0, 0);
                }
            }
        }
       renderPass->end(&_cmdBuffers[0]);
    }

    if (!_cmdBuffers[0].end()) return false;

    return _presentQueue->submit(_cmdBuffers[0], {drawCompleteSemaphore}, {imageReadySemaphore}, {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT});
}

bool ForwardRenderTechnique::init(DescriptorPool* descriptorPool, const std::vector<std::unique_ptr<ImageView> >& imageViews) {
    _pipelines[Light::Type::DirectionalLight] = Pipeline::createGraphicsPipeline(_device, "shader.vert.spv", "shader-directional.frag.spv");
    _pipelines[Light::Type::PointLight] = Pipeline::createGraphicsPipeline(_device, "shader.vert.spv", "shader-point.frag.spv");
    _pipelines[Light::Type::Spotlight] = Pipeline::createGraphicsPipeline(_device, "shader.vert.spv", "shader-spot.frag.spv");

    if (!_pipelines[Light::Type::DirectionalLight] ||
        !_pipelines[Light::Type::PointLight] ||
        !_pipelines[Light::Type::Spotlight]) {
        return false;
    }

    // We assume all pipelines have got the same layout
    // TODO: Create one DescriptorSetLayout for the pipelines
    std::vector<VkDescriptorSetLayout> descriptorSetLayouts(50, *_pipelines[Light::Type::DirectionalLight]->getLayout()->getDescriptorSetLayouts()[1]);
    _lightsDescriptorSets = descriptorPool->createDescriptorSets(descriptorSetLayouts);
    if (_lightsDescriptorSets.size() == 0) {
        return false;
    }

    _cmdBuffers = _presentQueue->getCommandPool().createCommandBuffers();
    if (_cmdBuffers.size() == 0) {
        return false;
    }

    return initDepthBuffers(imageViews) && initFramebuffers(imageViews) && initLightsBuffers() && initCameraBuffer(descriptorPool);
}

void ForwardRenderTechnique::destroy() {
    _presentQueue->waitIdle();

    for (auto& pipeline: _pipelines) {
        pipeline.second->destroy();
    }

    for (auto& cmdBuffer: _cmdBuffers) {
        cmdBuffer.destroy();
    }

    _framebuffers.clear();
}

bool ForwardRenderTechnique::initLightsBuffers() {
    uint32_t familyIndices = _presentQueue->getFamilyIdx();
    uint32_t lightsBufferSize = sizeof(Spotlight::LightData);
    bool memoryInitialized = false;
    _lightsBuffers.resize(50);

    uint32_t memoryOffset = 0;
    for (uint32_t i = 0; i < 50; ++i) {
        // Create buffer
        _lightsBuffers[i] = Buffer::create(_device, 1, &familyIndices, lightsBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!_lightsBuffers[i]) {
            return false;
        }

        // Allocate memory for all buffers (One time)
        auto& lightBuffer = _lightsBuffers[i];
        const VkMemoryRequirements* bufferRequirements = &lightBuffer->getRequirements();
        if (!memoryInitialized) {
            uint32_t memoryTypeIndex = DeviceMemory::findMemoryType(_device, *bufferRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            _lightsBuffersMemory = DeviceMemory::allocate(_device, bufferRequirements->size, memoryTypeIndex);
            if (!_lightsBuffersMemory) {
                return false;
            }
            memoryInitialized = true;
        }

        // Bind buffer memory
        lightBuffer->bindMemory(_lightsBuffersMemory.get(), memoryOffset);

        memoryOffset += (uint32_t)bufferRequirements->size;
    }

    return true;
}

bool ForwardRenderTechnique::initCameraBuffer(DescriptorPool* descriptorPool) {
    uint32_t familyIndices = _presentQueue->getFamilyIdx();

    // Create buffer
    _cameraBuffer = Buffer::create(_device, 1, &familyIndices, sizeof(Math::Mat4x4f) * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!_cameraBuffer) {
        return false;
    }

    // Allocate memory for the camera buffer
    const VkMemoryRequirements* bufferRequirements = &_cameraBuffer->getRequirements();
    uint32_t memoryTypeIndex = DeviceMemory::findMemoryType(_device, *bufferRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    _cameraBufferMemory = DeviceMemory::allocate(_device, bufferRequirements->size, memoryTypeIndex);
    if (!_cameraBufferMemory) {
        return false;
    }

    // Bind buffer memory
    _cameraBuffer->bindMemory(_cameraBufferMemory.get(), 0);

    // Create camera descriptor set
    // WARNING: it will only works if the pipelines has got the same descriptor set layout
    auto& descriptorSetLayout = _pipelines[Light::Type::DirectionalLight]->getLayout()->getDescriptorSetLayouts()[0];
    std::vector<DescriptorSet> descriptorSets = descriptorPool->createDescriptorSets({*descriptorSetLayout});
    if (descriptorSets.size() == 0) {
        return false;
    }
    _cameraDescriptorSet = std::move(descriptorSets[0]);

    // Assign camera buffer to descriptor set binding point 0
    _cameraDescriptorSet.update(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER , 0, _cameraBuffer.get(), 0, sizeof(Math::Mat4x4f) * 2);

    return true;
}

bool ForwardRenderTechnique::initDepthBuffers(const std::vector<std::unique_ptr<ImageView> >& imageViews) {
    VkFormat imagesFormat = Image::findSupportedFormat(_device,
                                                       {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                                                       VK_IMAGE_TILING_OPTIMAL,
                                                       VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (imagesFormat == VK_FORMAT_UNDEFINED) {
        LUG_LOG.error("ForwardRenderTechnique: Can't find supported format for depth buffer");
        return false;
    }

    if (_depthBufferMemory) {
        _depthBufferMemory.reset();
    }

    _depthBuffers.resize(imageViews.size());
    for (uint32_t i = 0; i < imageViews.size(); ++i) {
        std::unique_ptr<Image> image = nullptr;
        std::unique_ptr<ImageView> imageView = nullptr;

        VkExtent3D extent {
            extent.width = imageViews[i]->getExtent().width,
            extent.height = imageViews[i]->getExtent().height,
            extent.depth = 1
        };

        // Create depth buffer image
        {
            image = Image::create(_device, imagesFormat, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            if (!image) {
                LUG_LOG.error("ForwardRenderTechnique: Can't create depth buffer image");
                return false;
            }

            auto& imageRequirements = image->getRequirements();

            // Initialize depth buffer memory (This memory is common for all depth buffer images)
            if (!_depthBufferMemory) {
                uint32_t memoryTypeIndex = DeviceMemory::findMemoryType(_device, imageRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                // Allocate image requirements size for all images
                _depthBufferMemory = DeviceMemory::allocate(_device, imageRequirements.size * imageViews.size(), memoryTypeIndex);
                if (!_depthBufferMemory) {
                    LUG_LOG.error("ForwardRenderTechnique: Can't allocate device memory for depth buffer images");
                    return false;
                }
            }

            // Bind memory to image
            image->bindMemory(_depthBufferMemory.get(), imageRequirements.size * i);
        }

        // Create depth buffer image view
        {
            imageView = ImageView::create(_device, image.get(), imagesFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!imageView) {
                LUG_LOG.error("ForwardRenderTechnique: Can't create depth buffer image view");
                return false;
            }
        }

        _depthBuffers[i].image = std::move(image);
        _depthBuffers[i].imageView = std::move(imageView);
    }
    return true;
}

bool ForwardRenderTechnique::initFramebuffers(const std::vector<std::unique_ptr<ImageView> >& imageViews) {
    // The lights pipelines renderpass are compatible, so we don't need to create different frame buffers for each pipeline
    RenderPass* renderPass = _pipelines[Light::Type::DirectionalLight]->getRenderPass();

    VkResult result;
    _framebuffers.clear();
    _framebuffers.resize(imageViews.size());

    for (size_t i = 0; i < imageViews.size(); i++) {
        VkImageView attachments[2]{
            *imageViews[i],
            *_depthBuffers[i].imageView
        };

        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = *renderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = imageViews[i]->getExtent().width;
        framebufferInfo.height = imageViews[i]->getExtent().height;
        framebufferInfo.layers = 1;

        VkFramebuffer fb;
        result = vkCreateFramebuffer(*_device, &framebufferInfo, nullptr, &fb);
        if (result != VK_SUCCESS) {
            LUG_LOG.error("RendererVulkan: Failed to create framebuffer: {}", result);
            return false;
        }
        // TODO: Remove the extent initializer list when struct Extent is externalised
        _framebuffers[i] = Framebuffer(fb, _device, {imageViews[i]->getExtent().width, imageViews[i]->getExtent().height});
    }
    return true;
}

} // Vulkan
} // Graphics
} // lug
