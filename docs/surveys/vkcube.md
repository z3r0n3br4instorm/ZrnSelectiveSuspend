# Vulkan survey: vkcube

Run with `vkcube`, stopped after 4 s. 1 process(es) used Vulkan.

## `vkcube`

- api: **1.0**
- gpu: **NVIDIA GeForce GT 650M**
- driver-api: **1.2**
- application name: vkcube; engine: vkcube

### Extensions enabled (instance)

- `VK_KHR_display` — **not offered by ZSS_AirLock**
- `VK_KHR_get_physical_device_properties2` (core in 1.1) — **not offered by ZSS_AirLock**
- `VK_KHR_portability_enumeration` — **not offered by ZSS_AirLock**
- `VK_KHR_surface`
- `VK_KHR_wayland_surface`
- `VK_KHR_xcb_surface`
- `VK_KHR_xlib_surface`

### Extensions enabled (device)

- `VK_KHR_swapchain`

### Commands called: 62 different, 0 not in ZSS_AirLock

| Command | Calls | Since | In the layer |
| :--- | ---: | :--- | :--- |
| `vkBeginCommandBuffer` | 295 | 1.0 | yes |
| `vkEndCommandBuffer` | 295 | 1.0 | yes |
| `vkQueueSubmit` | 295 | 1.0 | yes |
| `vkWaitForFences` | 295 | 1.0 | yes |
| `vkAcquireNextImageKHR` | 294 | VK_KHR_swapchain | yes |
| `vkCmdBeginRenderPass` | 294 | 1.0 | yes |
| `vkCmdBindDescriptorSets` | 294 | 1.0 | yes |
| `vkCmdBindPipeline` | 294 | 1.0 | yes |
| `vkCmdDraw` | 294 | 1.0 | yes |
| `vkCmdEndRenderPass` | 294 | 1.0 | yes |
| `vkCmdSetScissor` | 294 | 1.0 | yes |
| `vkCmdSetViewport` | 294 | 1.0 | yes |
| `vkQueuePresentKHR` | 294 | VK_KHR_swapchain | yes |
| `vkResetCommandBuffer` | 294 | 1.0 | yes |
| `vkResetFences` | 294 | 1.0 | yes |
| `vkEnumerateDeviceExtensionProperties` | 16 | 1.0 | yes |
| `vkGetPhysicalDeviceProperties` | 11 | 1.0 | yes |
| `vkCreateImageView` | 5 | 1.0 | yes |
| `vkCreateSemaphore` | 5 | 1.0 | yes |
| `vkGetPhysicalDeviceSurfaceSupportKHR` | 5 | VK_KHR_surface | yes |
| `vkAllocateMemory` | 4 | 1.0 | yes |
| `vkEnumeratePhysicalDevices` | 4 | 1.0 | yes |
| `vkAllocateCommandBuffers` | 3 | 1.0 | yes |
| `vkCreateFence` | 3 | 1.0 | yes |
| `vkCreateFramebuffer` | 3 | 1.0 | yes |
| `vkMapMemory` | 3 | 1.0 | yes |
| `vkAllocateDescriptorSets` | 2 | 1.0 | yes |
| `vkBindBufferMemory` | 2 | 1.0 | yes |
| `vkBindImageMemory` | 2 | 1.0 | yes |
| `vkCreateBuffer` | 2 | 1.0 | yes |
| `vkCreateImage` | 2 | 1.0 | yes |
| `vkCreateShaderModule` | 2 | 1.0 | yes |
| `vkDestroyShaderModule` | 2 | 1.0 | yes |
| `vkGetBufferMemoryRequirements` | 2 | 1.0 | yes |
| `vkGetImageMemoryRequirements` | 2 | 1.0 | yes |
| `vkGetPhysicalDeviceQueueFamilyProperties` | 2 | 1.0 | yes |
| `vkGetPhysicalDeviceSurfaceFormatsKHR` | 2 | VK_KHR_surface | yes |
| `vkGetPhysicalDeviceSurfacePresentModesKHR` | 2 | VK_KHR_surface | yes |
| `vkGetSwapchainImagesKHR` | 2 | VK_KHR_swapchain | yes |
| `vkUpdateDescriptorSets` | 2 | 1.0 | yes |
| `vkCmdPipelineBarrier` | 1 | 1.0 | yes |
| `vkCreateCommandPool` | 1 | 1.0 | yes |
| `vkCreateDescriptorPool` | 1 | 1.0 | yes |
| `vkCreateDescriptorSetLayout` | 1 | 1.0 | yes |
| `vkCreateDevice` | 1 | 1.0 | yes |
| `vkCreateGraphicsPipelines` | 1 | 1.0 | yes |
| `vkCreateInstance` | 1 | 1.0 | yes |
| `vkCreatePipelineCache` | 1 | 1.0 | yes |
| `vkCreatePipelineLayout` | 1 | 1.0 | yes |
| `vkCreateRenderPass` | 1 | 1.0 | yes |
| `vkCreateSampler` | 1 | 1.0 | yes |
| `vkCreateSwapchainKHR` | 1 | VK_KHR_swapchain | yes |
| `vkCreateXcbSurfaceKHR` | 1 | VK_KHR_xcb_surface | yes |
| `vkDestroyFence` | 1 | 1.0 | yes |
| `vkFreeCommandBuffers` | 1 | 1.0 | yes |
| `vkGetDeviceQueue` | 1 | 1.0 | yes |
| `vkGetImageSubresourceLayout` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceFeatures` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceFormatProperties` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceMemoryProperties` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` | 1 | VK_KHR_surface | yes |
| `vkUnmapMemory` | 1 | 1.0 | yes |

