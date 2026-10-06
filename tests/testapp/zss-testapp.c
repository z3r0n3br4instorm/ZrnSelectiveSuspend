// SPDX-License-Identifier: GPL-2.0-only
/*
 * zss-testapp: a deterministic off-screen renderer for migration tests.
 *
 * Every frame is a pure function of its index, so a run that was migrated
 * part-way must produce the same files as one that was not. The scene is
 * chosen to depend on state that only survives if migration is correct:
 *
 *   - a history image that receives one more stamp each frame and is never
 *     cleared (contents generated on the GPU, carried across frames)
 *   - a texture uploaded once at start-up
 *   - a uniform buffer kept mapped for the whole run
 *   - a depth buffer
 *   - a command buffer recorded once and reused
 *
 * --forget-history-at N zeroes the history image before frame N. That is the
 * picture a run is expected to produce after losing its GPU at frame N, since
 * the history is the one thing here that only the GPU ever had.
 *
 * Frames are written as raw RGBA8 to <out>/frame_NNNN.rgba.
 */
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "quad_frag.h"
#include "quad_vert.h"

#define SIZE 256
#define TEX 64
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) die(#call, r_); } while (0)

struct frame_ubo {
    float stamp_rect[4], stamp_color[4], badge_rect[4], badge_tint[4];
};

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties memprops;

static void die(const char *what, VkResult r)
{
    fprintf(stderr, "zss-testapp: %s failed (VkResult %d)\n", what, r);
    exit(1);
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    die("finding a memory type", VK_ERROR_FEATURE_NOT_PRESENT);
    return 0;
}

static VkDeviceMemory alloc_for(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, want),
    };
    VkDeviceMemory mem;

    CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
    return mem;
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem)
{
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    VkMemoryRequirements req;
    VkBuffer buf;

    CHECK(vkCreateBuffer(dev, &ci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &req);
    *mem = alloc_for(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(dev, buf, *mem, 0));
    return buf;
}

static VkImage make_image(VkFormat format, uint32_t size, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                          VkImageView *view)
{
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { size, size, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    VkMemoryRequirements req;
    VkImage img;

    CHECK(vkCreateImage(dev, &ci, NULL, &img));
    vkGetImageMemoryRequirements(dev, img, &req);
    CHECK(vkBindImageMemory(dev, img, alloc_for(req, 0), 0));
    vi.image = img;
    CHECK(vkCreateImageView(dev, &vi, NULL, view));
    return img;
}

static void layout(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };

    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         NULL, 0, NULL, 1, &b);
}

static void run_once(VkCommandBuffer cb)
{
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };

    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
}

static VkRenderPass make_pass(VkAttachmentLoadOp load, VkImageLayout initial, bool depth)
{
    VkAttachmentDescription att[2] = {
        {
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = load,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = initial,
            .finalLayout = depth ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        {
            .format = VK_FORMAT_D16_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ds = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthStencilAttachment = depth ? &ds : NULL,
    };
    VkRenderPassCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = depth ? 2 : 1,
        .pAttachments = att,
        .subpassCount = 1,
        .pSubpasses = &sub,
    };
    VkRenderPass rp;

    CHECK(vkCreateRenderPass(dev, &ci, NULL, &rp));
    return rp;
}

static VkPipeline make_pipeline(VkRenderPass rp, VkPipelineLayout pl, bool depth)
{
    VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof(quad_vert_spv), .pCode = quad_vert_spv };
    VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = sizeof(quad_frag_spv), .pCode = quad_frag_spv };
    VkShaderModule vs, fs;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" },
    };
    VkVertexInputBindingDescription bind = { 0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = { 0, 0, SIZE, SIZE, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { SIZE, SIZE } };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth, .depthWriteEnable = depth, .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &dss, .pColorBlendState = &cb, .layout = pl, .renderPass = rp,
    };
    VkPipeline p;

    CHECK(vkCreateShaderModule(dev, &vsi, NULL, &vs));
    CHECK(vkCreateShaderModule(dev, &fsi, NULL, &fs));
    stages[0].module = vs;
    stages[1].module = fs;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    /* Destroyed on purpose: the layer must still be able to rebuild the pipeline. */
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    return p;
}

/* Clip-space rectangle for a pixel-aligned square, so every driver rasterises it identically. */
static void rect(float *out, int x, int y, int size)
{
    out[0] = 2.0f * (float)x / SIZE - 1.0f;
    out[1] = 2.0f * (float)y / SIZE - 1.0f;
    out[2] = 2.0f * (float)size / SIZE;
    out[3] = out[2];
}

static void fill_frame(struct frame_ubo *u, int f)
{
    static const float palette[6][3] = {
        { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 },
    };

    rect(u->stamp_rect, (f * 24) % 240, ((f * 24) / 240 * 24 + f * 3) % 240, 16);
    memcpy(u->stamp_color, palette[f % 6], 3 * sizeof(float));
    u->stamp_color[3] = 1.0f;
    rect(u->badge_rect, (f * 7) % 192, 192 - (f * 5) % 192, TEX);
    u->badge_tint[0] = (f % 3 == 0) ? 1.0f : 0.5f;
    u->badge_tint[1] = (f % 3 == 1) ? 1.0f : 0.5f;
    u->badge_tint[2] = (f % 3 == 2) ? 1.0f : 0.5f;
    u->badge_tint[3] = 1.0f;
}

int main(int argc, char **argv)
{
    const char *gpu = getenv("ZSS_TESTAPP_GPU"), *out = ".";
    int frames = 40, delay_ms = 0, forget_at = -1;
    bool all_features = false, untracked = false, list = false;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "zss-testapp",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkPhysicalDevice pds[16];
    VkPhysicalDeviceFeatures features = { 0 };
    VkQueueFamilyProperties qf[16];
    uint32_t npd = 16, nqf = 16;
    VkInstance inst;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gpu") && i + 1 < argc) gpu = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--forget-history-at") && i + 1 < argc) forget_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--enable-all-features")) all_features = true;
        else if (!strcmp(argv[i], "--use-untracked")) untracked = true;
        else if (!strcmp(argv[i], "--list")) list = true;
        else {
            fputs("usage: zss-testapp [--gpu NAME] [--out DIR] [--frames N] [--delay-ms N]\n"
                  "                   [--enable-all-features] [--use-untracked] [--list]\n"
                  "                   [--forget-history-at N]\n", stderr);
            return 2;
        }
    }

    CHECK(vkCreateInstance(&ici, NULL, &inst));
    CHECK(vkEnumeratePhysicalDevices(inst, &npd, pds));
    if (list) {
        for (uint32_t i = 0; i < npd; i++) {
            VkPhysicalDeviceProperties p;

            vkGetPhysicalDeviceProperties(pds[i], &p);
            puts(p.deviceName);
        }
        return 0;
    }
    pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties p;

        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (!gpu || strstr(p.deviceName, gpu)) {
            pd = pds[i];
            fprintf(stderr, "zss-testapp: using %s\n", p.deviceName);
            break;
        }
    }
    if (!pd) {
        fprintf(stderr, "zss-testapp: no GPU matching '%s'\n", gpu ? gpu : "");
        return 3;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
    for (family = 0; family < nqf; family++)
        if (qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (all_features)
        vkGetPhysicalDeviceFeatures(pd, &features);
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &qci, .pEnabledFeatures = &features };

        CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    }
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = family };
    VkCommandPool pool;
    VkCommandBuffer cbs[2];
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, cbs));
    VkCommandBuffer setup = cbs[0], frame_cb = cbs[1];
    VkCommandBufferBeginInfo once = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkCommandBufferBeginInfo reuse = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    /* Images. */
    VkImageView history_view, final_view, depth_view, tex_view;
    VkImage history = make_image(VK_FORMAT_R8G8B8A8_UNORM, SIZE,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, &history_view);
    VkImage final = make_image(VK_FORMAT_R8G8B8A8_UNORM, SIZE,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, &final_view);
    make_image(VK_FORMAT_D16_UNORM, SIZE, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
               &depth_view);
    VkImage tex = make_image(VK_FORMAT_R8G8B8A8_UNORM, TEX,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &tex_view);

    /* Texture upload through a staging buffer that is then thrown away. */
    VkDeviceMemory staging_mem, vb_mem, ubo_mem, read_mem;
    VkBuffer staging = make_buffer(TEX * TEX * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_mem);
    uint8_t *px;
    CHECK(vkMapMemory(dev, staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    for (int y = 0; y < TEX; y++) {
        for (int x = 0; x < TEX; x++) {
            bool on = ((x / 8) + (y / 8)) & 1;

            px[(y * TEX + x) * 4 + 0] = on ? 255 : (uint8_t)(x * 4);
            px[(y * TEX + x) * 4 + 1] = on ? 255 : (uint8_t)(y * 4);
            px[(y * TEX + x) * 4 + 2] = on ? 255 : 64;
            px[(y * TEX + x) * 4 + 3] = 255;
        }
    }
    vkUnmapMemory(dev, staging_mem);

    CHECK(vkBeginCommandBuffer(setup, &once));
    VkBufferImageCopy up = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TEX, TEX, 1 } };
    VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    layout(setup, tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(setup, staging, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
    layout(setup, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    layout(setup, history, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(setup, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    layout(setup, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    run_once(setup);
    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);

    /* Buffers. The vertex buffer is written once; the uniform and read-back buffers stay mapped. */
    static const float quad[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    VkBuffer vb = make_buffer(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb_mem);
    VkBuffer ubo = make_buffer(sizeof(struct frame_ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo_mem);
    VkBuffer readback = make_buffer(SIZE * SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_mem);
    struct frame_ubo *ubo_map;
    uint8_t *read_map;
    void *p;
    CHECK(vkMapMemory(dev, vb_mem, 0, VK_WHOLE_SIZE, 0, &p));
    memcpy(p, quad, sizeof(quad));
    vkUnmapMemory(dev, vb_mem);
    CHECK(vkMapMemory(dev, ubo_mem, 0, VK_WHOLE_SIZE, 0, (void **)&ubo_map));
    CHECK(vkMapMemory(dev, read_mem, 0, VK_WHOLE_SIZE, 0, (void **)&read_map));

    /* Passes, framebuffers, descriptors, pipelines. */
    VkRenderPass history_pass = make_pass(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    VkRenderPass final_pass = make_pass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, true);
    VkImageView final_att[2] = { final_view, depth_view };
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = history_pass,
                                    .attachmentCount = 1, .pAttachments = &history_view,
                                    .width = SIZE, .height = SIZE, .layers = 1 };
    VkFramebuffer history_fb, final_fb;
    CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &history_fb));
    fbi.renderPass = final_pass;
    fbi.attachmentCount = 2;
    fbi.pAttachments = final_att;
    CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &final_fb));

    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler sampler;
    CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));

    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
                                      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
                                       .poolSizeCount = 2, .pPoolSizes = sizes };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkDescriptorSetLayout two[2] = { dsl, dsl };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dpool, .descriptorSetCount = 2, .pSetLayouts = two };
    VkDescriptorSet sets[2]; /* 0 samples the texture, 1 samples the history image */
    CHECK(vkAllocateDescriptorSets(dev, &dai, sets));
    VkDescriptorBufferInfo dbi = { ubo, 0, sizeof(struct frame_ubo) };
    VkDescriptorImageInfo dii[2] = {
        { sampler, tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { sampler, history_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet writes[4];
    for (int i = 0; i < 2; i++) {
        writes[i * 2] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i],
                                                .dstBinding = 0, .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                .pBufferInfo = &dbi };
        writes[i * 2 + 1] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i],
                                                    .dstBinding = 1, .descriptorCount = 1,
                                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                    .pImageInfo = &dii[i] };
    }
    vkUpdateDescriptorSets(dev, 4, writes, 0, NULL);

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkPipeline history_pipe = make_pipeline(history_pass, pl, false);
    VkPipeline final_pipe = make_pipeline(final_pass, pl, true);

    VkQueryPool qpool = VK_NULL_HANDLE;
    if (untracked) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1 };

        CHECK(vkCreateQueryPool(dev, &qi, NULL, &qpool));
    }

    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fi, NULL, &fence));

    puts("READY");
    fflush(stdout);

    for (int f = 0; f < frames; f++) {
        /* Recorded on the first frame and again every eighth, reused in between. */
        if (f % 8 == 0) {
            VkClearValue clears[2] = { { .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } },
                                       { .depthStencil = { 1.0f, 0 } } };
            VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                          .renderArea = { { 0, 0 }, { SIZE, SIZE } } };
            VkBufferImageCopy down = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                       .imageExtent = { SIZE, SIZE, 1 } };
            VkDeviceSize zero = 0;
            int mode;

            CHECK(vkResetCommandBuffer(frame_cb, 0));
            CHECK(vkBeginCommandBuffer(frame_cb, &reuse));
            vkCmdBindVertexBuffers(frame_cb, 0, 1, &vb, &zero);

            rbi.renderPass = history_pass;
            rbi.framebuffer = history_fb;
            vkCmdBeginRenderPass(frame_cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, history_pipe);
            vkCmdBindDescriptorSets(frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[0], 0, NULL);
            mode = 0;
            vkCmdPushConstants(frame_cb, pl, pcr.stageFlags, 0, sizeof(mode), &mode);
            vkCmdDraw(frame_cb, 6, 1, 0, 0);
            vkCmdEndRenderPass(frame_cb);

            rbi.renderPass = final_pass;
            rbi.framebuffer = final_fb;
            rbi.clearValueCount = 2;
            rbi.pClearValues = clears;
            vkCmdBeginRenderPass(frame_cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, final_pipe);
            vkCmdBindDescriptorSets(frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[1], 0, NULL);
            mode = 1;
            vkCmdPushConstants(frame_cb, pl, pcr.stageFlags, 0, sizeof(mode), &mode);
            vkCmdDraw(frame_cb, 6, 1, 0, 0);
            vkCmdBindDescriptorSets(frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[0], 0, NULL);
            mode = 2;
            vkCmdPushConstants(frame_cb, pl, pcr.stageFlags, 0, sizeof(mode), &mode);
            vkCmdDraw(frame_cb, 6, 1, 0, 0);
            vkCmdEndRenderPass(frame_cb);

            vkCmdCopyImageToBuffer(frame_cb, final, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &down);
            CHECK(vkEndCommandBuffer(frame_cb));
        }

        if (f == forget_at) {
            VkClearColorValue nothing = { .float32 = { 0, 0, 0, 0 } };

            CHECK(vkBeginCommandBuffer(setup, &once));
            layout(setup, history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(setup, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &nothing, 1, &whole);
            layout(setup, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            run_once(setup);
        }

        fill_frame(ubo_map, f);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                            .pCommandBuffers = &frame_cb };
        CHECK(vkResetFences(dev, 1, &fence));
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%04d.rgba", out, f);
        FILE *fp = fopen(path, "wb");
        if (!fp || fwrite(read_map, 1, SIZE * SIZE * 4, fp) != SIZE * SIZE * 4) {
            fprintf(stderr, "zss-testapp: cannot write %s\n", path);
            return 1;
        }
        fclose(fp);
        printf("frame %d\n", f);
        fflush(stdout);
        if (delay_ms)
            usleep((useconds_t)delay_ms * 1000);
    }

    CHECK(vkDeviceWaitIdle(dev));
    if (qpool)
        vkDestroyQueryPool(dev, qpool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    puts("DONE");
    return 0;
}
