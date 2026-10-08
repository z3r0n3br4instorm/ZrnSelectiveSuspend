# Vulkan survey: Firefox on Zink

Run on Zink with `firefox --no-remote --profile /tmp/claude-1000/-home-zerone-Documents-Projects-ZrnSelectiveSuspend/6179ab03-5bda-4e04-8b0e-688db1e3a7be/scratchpad/ff3 file:///tmp/claude-1000/-home-zerone-Documents-Projects-ZrnSelectiveSuspend/6179ab03-5bda-4e04-8b0e-688db1e3a7be/scratchpad/anim.html`, stopped after 22 s. 1 process(es) used Vulkan.

## `gfxtest`

- api: **1.4**
- gpu: **Intel(R) HD Graphics 4000 (IVB GT2)**
- driver-api: **1.2**
- application name: gfxtest; engine: mesa zink

### Extensions enabled (instance)

- `VK_EXT_debug_utils` — **not offered by the ZSS layer**
- `VK_EXT_headless_surface` — **not offered by the ZSS layer**
- `VK_EXT_layer_settings` — **not offered by the ZSS layer**
- `VK_EXT_swapchain_colorspace` — **not offered by the ZSS layer**
- `VK_KHR_external_memory_capabilities` (core in 1.1) — **not offered by the ZSS layer**
- `VK_KHR_external_semaphore_capabilities` (core in 1.1) — **not offered by the ZSS layer**
- `VK_KHR_get_physical_device_properties2` (core in 1.1) — **not offered by the ZSS layer**
- `VK_KHR_surface`
- `VK_KHR_wayland_surface`
- `VK_KHR_xcb_surface`

### Extensions enabled (device)

- `VK_EXT_4444_formats` (core in 1.3) — **not offered by the ZSS layer**
- `VK_EXT_color_write_enable` — **not offered by the ZSS layer**
- `VK_EXT_depth_clamp_zero_one` (core in VK.KHR.depth.clamp.zero.one) — **not offered by the ZSS layer**
- `VK_EXT_depth_clip_control` — **not offered by the ZSS layer**
- `VK_EXT_depth_clip_enable` — **not offered by the ZSS layer**
- `VK_EXT_extended_dynamic_state` (core in 1.3) — **not offered by the ZSS layer**
- `VK_EXT_extended_dynamic_state2` (core in 1.3) — **not offered by the ZSS layer**
- `VK_EXT_external_memory_dma_buf` — **not offered by the ZSS layer**
- `VK_EXT_external_memory_host` — **not offered by the ZSS layer**
- `VK_EXT_image_2d_view_of_3d` — **not offered by the ZSS layer**
- `VK_EXT_image_drm_format_modifier` — **not offered by the ZSS layer**
- `VK_EXT_image_robustness` (core in 1.3) — **not offered by the ZSS layer**
- `VK_EXT_index_type_uint8` (core in VK.KHR.index.type.uint8) — **not offered by the ZSS layer**
- `VK_EXT_line_rasterization` (core in VK.KHR.line.rasterization) — **not offered by the ZSS layer**
- `VK_EXT_memory_budget` — **not offered by the ZSS layer**
- `VK_EXT_multi_draw` — **not offered by the ZSS layer**
- `VK_EXT_non_seamless_cube_map` — **not offered by the ZSS layer**
- `VK_EXT_pci_bus_info` — **not offered by the ZSS layer**
- `VK_EXT_pipeline_creation_cache_control` (core in 1.3) — **not offered by the ZSS layer**
- `VK_EXT_primitive_topology_list_restart` — **not offered by the ZSS layer**
- `VK_EXT_primitives_generated_query` — **not offered by the ZSS layer**
- `VK_EXT_provoking_vertex` — **not offered by the ZSS layer**
- `VK_EXT_queue_family_foreign` — **not offered by the ZSS layer**
- `VK_EXT_sample_locations` — **not offered by the ZSS layer**
- `VK_EXT_shader_atomic_float` — **not offered by the ZSS layer**
- `VK_EXT_shader_demote_to_helper_invocation` (core in 1.3) — **not offered by the ZSS layer**
- `VK_EXT_shader_subgroup_ballot` — **not offered by the ZSS layer**
- `VK_EXT_shader_subgroup_vote` — **not offered by the ZSS layer**
- `VK_EXT_transform_feedback` — **not offered by the ZSS layer**
- `VK_EXT_vertex_attribute_divisor` (core in VK.KHR.vertex.attribute.divisor) — **not offered by the ZSS layer**
- `VK_KHR_calibrated_timestamps` — **not offered by the ZSS layer**
- `VK_KHR_dynamic_rendering` (core in 1.3) — **not offered by the ZSS layer**
- `VK_KHR_external_memory_fd` — **not offered by the ZSS layer**
- `VK_KHR_external_semaphore_fd` — **not offered by the ZSS layer**
- `VK_KHR_format_feature_flags2` (core in 1.3) — **not offered by the ZSS layer**
- `VK_KHR_incremental_present` — **not offered by the ZSS layer**
- `VK_KHR_maintenance4` (core in 1.3) — **not offered by the ZSS layer**
- `VK_KHR_maintenance5` (core in 1.4) — **not offered by the ZSS layer**
- `VK_KHR_maintenance6` (core in 1.4) — **not offered by the ZSS layer**
- `VK_KHR_pipeline_executable_properties` — **not offered by the ZSS layer**
- `VK_KHR_push_descriptor` (core in 1.4) — **not offered by the ZSS layer**
- `VK_KHR_robustness2` — **not offered by the ZSS layer**
- `VK_KHR_shader_clock` — **not offered by the ZSS layer**
- `VK_KHR_swapchain`
- `VK_KHR_swapchain_mutable_format` — **not offered by the ZSS layer**
- `VK_KHR_synchronization2` (core in 1.3) — **not offered by the ZSS layer**
- `VK_KHR_workgroup_memory_explicit_layout` — **not offered by the ZSS layer**
- `VK_NV_compute_shader_derivatives` (core in VK.KHR.compute.shader.derivatives) — **not offered by the ZSS layer**

### Features enabled

- `VkPhysicalDevice4444FormatsFeaturesEXT.formatA4R4G4B4`
- `VkPhysicalDeviceColorWriteEnableFeaturesEXT.colorWriteEnable`
- `VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR.computeDerivativeGroupLinear`
- `VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR.computeDerivativeGroupQuads`
- `VkPhysicalDeviceDepthClampZeroOneFeaturesKHR.depthClampZeroOne`
- `VkPhysicalDeviceDepthClipControlFeaturesEXT.depthClipControl`
- `VkPhysicalDeviceDepthClipEnableFeaturesEXT.depthClipEnable`
- `VkPhysicalDeviceDynamicRenderingFeatures.dynamicRendering`
- `VkPhysicalDeviceExtendedDynamicState2FeaturesEXT.extendedDynamicState2`
- `VkPhysicalDeviceExtendedDynamicState2FeaturesEXT.extendedDynamicState2LogicOp`
- `VkPhysicalDeviceExtendedDynamicStateFeaturesEXT.extendedDynamicState`
- `VkPhysicalDeviceFeatures.alphaToOne`
- `VkPhysicalDeviceFeatures.depthBiasClamp`
- `VkPhysicalDeviceFeatures.depthClamp`
- `VkPhysicalDeviceFeatures.drawIndirectFirstInstance`
- `VkPhysicalDeviceFeatures.dualSrcBlend`
- `VkPhysicalDeviceFeatures.fillModeNonSolid`
- `VkPhysicalDeviceFeatures.fragmentStoresAndAtomics`
- `VkPhysicalDeviceFeatures.fullDrawIndexUint32`
- `VkPhysicalDeviceFeatures.geometryShader`
- `VkPhysicalDeviceFeatures.imageCubeArray`
- `VkPhysicalDeviceFeatures.independentBlend`
- `VkPhysicalDeviceFeatures.inheritedQueries`
- `VkPhysicalDeviceFeatures.largePoints`
- `VkPhysicalDeviceFeatures.logicOp`
- `VkPhysicalDeviceFeatures.multiDrawIndirect`
- `VkPhysicalDeviceFeatures.multiViewport`
- `VkPhysicalDeviceFeatures.occlusionQueryPrecise`
- `VkPhysicalDeviceFeatures.pipelineStatisticsQuery`
- `VkPhysicalDeviceFeatures.robustBufferAccess`
- `VkPhysicalDeviceFeatures.sampleRateShading`
- `VkPhysicalDeviceFeatures.samplerAnisotropy`
- `VkPhysicalDeviceFeatures.shaderClipDistance`
- `VkPhysicalDeviceFeatures.shaderCullDistance`
- `VkPhysicalDeviceFeatures.shaderImageGatherExtended`
- `VkPhysicalDeviceFeatures.shaderSampledImageArrayDynamicIndexing`
- `VkPhysicalDeviceFeatures.shaderStorageBufferArrayDynamicIndexing`
- `VkPhysicalDeviceFeatures.shaderStorageImageArrayDynamicIndexing`
- `VkPhysicalDeviceFeatures.shaderStorageImageExtendedFormats`
- `VkPhysicalDeviceFeatures.shaderStorageImageWriteWithoutFormat`
- `VkPhysicalDeviceFeatures.shaderTessellationAndGeometryPointSize`
- `VkPhysicalDeviceFeatures.shaderUniformBufferArrayDynamicIndexing`
- `VkPhysicalDeviceFeatures.tessellationShader`
- `VkPhysicalDeviceFeatures.textureCompressionBC`
- `VkPhysicalDeviceFeatures.variableMultisampleRate`
- `VkPhysicalDeviceFeatures.wideLines`
- `VkPhysicalDeviceImage2DViewOf3DFeaturesEXT.image2DViewOf3D`
- `VkPhysicalDeviceImageRobustnessFeatures.robustImageAccess`
- `VkPhysicalDeviceIndexTypeUint8Features.indexTypeUint8`
- `VkPhysicalDeviceLineRasterizationFeatures.bresenhamLines`
- `VkPhysicalDeviceLineRasterizationFeatures.smoothLines`
- `VkPhysicalDeviceLineRasterizationFeatures.stippledBresenhamLines`
- `VkPhysicalDeviceMaintenance4Features.maintenance4`
- `VkPhysicalDeviceMaintenance5Features.maintenance5`
- `VkPhysicalDeviceMaintenance6Features.maintenance6`
- `VkPhysicalDeviceMultiDrawFeaturesEXT.multiDraw`
- `VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT.nonSeamlessCubeMap`
- `VkPhysicalDevicePipelineCreationCacheControlFeatures.pipelineCreationCacheControl`
- `VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR.pipelineExecutableInfo`
- `VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT.primitiveTopologyListRestart`
- `VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT.primitiveTopologyPatchListRestart`
- `VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT.primitivesGeneratedQuery`
- `VkPhysicalDeviceProvokingVertexFeaturesEXT.provokingVertexLast`
- `VkPhysicalDeviceProvokingVertexFeaturesEXT.transformFeedbackPreservesProvokingVertex`
- `VkPhysicalDeviceRobustness2FeaturesKHR.nullDescriptor`
- `VkPhysicalDeviceRobustness2FeaturesKHR.robustBufferAccess2`
- `VkPhysicalDeviceRobustness2FeaturesKHR.robustImageAccess2`
- `VkPhysicalDeviceShaderAtomicFloatFeaturesEXT.shaderBufferFloat32Atomics`
- `VkPhysicalDeviceShaderAtomicFloatFeaturesEXT.shaderImageFloat32Atomics`
- `VkPhysicalDeviceShaderAtomicFloatFeaturesEXT.shaderSharedFloat32Atomics`
- `VkPhysicalDeviceShaderClockFeaturesKHR.shaderSubgroupClock`
- `VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures.shaderDemoteToHelperInvocation`
- `VkPhysicalDeviceSynchronization2Features.synchronization2`
- `VkPhysicalDeviceTransformFeedbackFeaturesEXT.geometryStreams`
- `VkPhysicalDeviceTransformFeedbackFeaturesEXT.transformFeedback`
- `VkPhysicalDeviceVertexAttributeDivisorFeatures.vertexAttributeInstanceRateDivisor`
- `VkPhysicalDeviceVertexAttributeDivisorFeatures.vertexAttributeInstanceRateZeroDivisor`
- `VkPhysicalDeviceVulkan11Features.multiview`
- `VkPhysicalDeviceVulkan11Features.multiviewGeometryShader`
- `VkPhysicalDeviceVulkan11Features.multiviewTessellationShader`
- `VkPhysicalDeviceVulkan11Features.samplerYcbcrConversion`
- `VkPhysicalDeviceVulkan11Features.shaderDrawParameters`
- `VkPhysicalDeviceVulkan11Features.variablePointers`
- `VkPhysicalDeviceVulkan11Features.variablePointersStorageBuffer`
- `VkPhysicalDeviceVulkan12Features.drawIndirectCount`
- `VkPhysicalDeviceVulkan12Features.hostQueryReset`
- `VkPhysicalDeviceVulkan12Features.imagelessFramebuffer`
- `VkPhysicalDeviceVulkan12Features.samplerMirrorClampToEdge`
- `VkPhysicalDeviceVulkan12Features.scalarBlockLayout`
- `VkPhysicalDeviceVulkan12Features.separateDepthStencilLayouts`
- `VkPhysicalDeviceVulkan12Features.shaderOutputLayer`
- `VkPhysicalDeviceVulkan12Features.shaderOutputViewportIndex`
- `VkPhysicalDeviceVulkan12Features.shaderSubgroupExtendedTypes`
- `VkPhysicalDeviceVulkan12Features.subgroupBroadcastDynamicId`
- `VkPhysicalDeviceVulkan12Features.timelineSemaphore`
- `VkPhysicalDeviceVulkan12Features.uniformBufferStandardLayout`
- `VkPhysicalDeviceVulkan12Features.vulkanMemoryModel`
- `VkPhysicalDeviceVulkan12Features.vulkanMemoryModelAvailabilityVisibilityChains`
- `VkPhysicalDeviceVulkan12Features.vulkanMemoryModelDeviceScope`
- `VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayout`
- `VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayout16BitAccess`
- `VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayout8BitAccess`
- `VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.workgroupMemoryExplicitLayoutScalarBlockLayout`

### Structures chained to the create calls

- `VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_KHR`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES`
- `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR`

### Commands called: 33 different, 10 not in the ZSS layer

| Command | Calls | Since | In the layer |
| :--- | ---: | :--- | :--- |
| `vkGetPhysicalDeviceFormatProperties2KHR` | 150 | VK_KHR_get_physical_device_properties2 | **no** |
| `vkGetPhysicalDeviceImageFormatProperties2KHR` | 91 | VK_KHR_get_physical_device_properties2 | **no** |
| `vkEnumerateDeviceExtensionProperties` | 10 | 1.0 | yes |
| `vkAllocateCommandBuffers` | 8 | 1.0 | yes |
| `vkCreateCommandPool` | 8 | 1.0 | yes |
| `vkGetPhysicalDeviceFormatProperties` | 8 | 1.0 | yes |
| `vkSetDebugUtilsObjectNameEXT` | 8 | VK_EXT_debug_utils | **no** |
| `vkBeginCommandBuffer` | 3 | 1.0 | yes |
| `vkCreateDescriptorSetLayout` | 3 | 1.0 | yes |
| `vkGetDescriptorSetLayoutSupport` | 3 | 1.1 | **no** |
| `vkGetPhysicalDeviceProperties2` | 3 | 1.1 | **no** |
| `vkAllocateMemory` | 2 | 1.0 | yes |
| `vkBindBufferMemory` | 2 | 1.0 | yes |
| `vkCmdSetColorWriteEnableEXT` | 2 | VK_EXT_color_write_enable | **no** |
| `vkCreateBuffer` | 2 | 1.0 | yes |
| `vkCreateSampler` | 2 | 1.0 | yes |
| `vkEnumeratePhysicalDevices` | 2 | 1.0 | yes |
| `vkGetBufferMemoryRequirements` | 2 | 1.0 | yes |
| `vkGetPhysicalDeviceCalibrateableTimeDomainsEXT` | 2 | VK_EXT_calibrated_timestamps | **no** |
| `vkGetPhysicalDeviceMultisamplePropertiesEXT` | 2 | VK_EXT_sample_locations | **no** |
| `vkGetPhysicalDeviceQueueFamilyProperties` | 2 | 1.0 | yes |
| `vkCmdCopyBuffer` | 1 | 1.0 | yes |
| `vkCreateDevice` | 1 | 1.0 | yes |
| `vkCreateInstance` | 1 | 1.0 | yes |
| `vkCreatePipelineLayout` | 1 | 1.0 | yes |
| `vkCreateSemaphore` | 1 | 1.0 | yes |
| `vkGetDeviceQueue` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceFeatures2KHR` | 1 | VK_KHR_get_physical_device_properties2 | **no** |
| `vkGetPhysicalDeviceImageFormatProperties` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceMemoryProperties` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceProperties` | 1 | 1.0 | yes |
| `vkGetPhysicalDeviceProperties2KHR` | 1 | VK_KHR_get_physical_device_properties2 | **no** |
| `vkMapMemory` | 1 | 1.0 | yes |

