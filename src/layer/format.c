// SPDX-License-Identifier: GPL-2.0
/*
 * Texel sizes, needed to copy image contents through buffers. A format that
 * is missing here makes its device non-migratable rather than risking a
 * wrong copy.
 */
#include "zss_layer.h"

bool zss_format_info(VkFormat f, VkImageAspectFlags aspect, struct zss_format_info *out)
{
    uint32_t bytes = 0, bw = 1, bh = 1;

    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
        switch (f) {
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            bytes = 1;
            break;
        default:
            break;
        }
        goto done;
    }
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            bytes = 2;
            break;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            bytes = 4;
            break;
        default:
            break;
        }
        goto done;
    }

    if (f >= VK_FORMAT_R4G4_UNORM_PACK8 && f <= VK_FORMAT_R4G4_UNORM_PACK8)
        bytes = 1;
    else if (f >= VK_FORMAT_R4G4B4A4_UNORM_PACK16 && f <= VK_FORMAT_A1R5G5B5_UNORM_PACK16)
        bytes = 2;
    else if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB)
        bytes = 1;
    else if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB)
        bytes = 2;
    else if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB)
        bytes = 3;
    else if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32)
        bytes = 4;
    else if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT)
        bytes = 2;
    else if (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT)
        bytes = 4;
    else if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT)
        bytes = 6;
    else if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT)
        bytes = 8;
    else if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT)
        bytes = 4;
    else if (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT)
        bytes = 8;
    else if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT)
        bytes = 12;
    else if (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT)
        bytes = 16;
    else if (f >= VK_FORMAT_R64_UINT && f <= VK_FORMAT_R64_SFLOAT)
        bytes = 8;
    else if (f >= VK_FORMAT_R64G64_UINT && f <= VK_FORMAT_R64G64_SFLOAT)
        bytes = 16;
    else if (f >= VK_FORMAT_R64G64B64_UINT && f <= VK_FORMAT_R64G64B64_SFLOAT)
        bytes = 24;
    else if (f >= VK_FORMAT_R64G64B64A64_UINT && f <= VK_FORMAT_R64G64B64A64_SFLOAT)
        bytes = 32;
    else if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
        bytes = 4;
    else if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
        bytes = 8, bw = bh = 4;
    else if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC3_SRGB_BLOCK)
        bytes = 16, bw = bh = 4;
    else if (f >= VK_FORMAT_BC4_UNORM_BLOCK && f <= VK_FORMAT_BC4_SNORM_BLOCK)
        bytes = 8, bw = bh = 4;
    else if (f >= VK_FORMAT_BC5_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK)
        bytes = 16, bw = bh = 4;
    else if (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK)
        bytes = 8, bw = bh = 4;
    else if (f >= VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK && f <= VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK)
        bytes = 16, bw = bh = 4;
    else if (f >= VK_FORMAT_EAC_R11_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11_SNORM_BLOCK)
        bytes = 8, bw = bh = 4;
    else if (f >= VK_FORMAT_EAC_R11G11_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK)
        bytes = 16, bw = bh = 4;
    else if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        static const uint8_t dims[14][2] = {
            { 4, 4 }, { 5, 4 }, { 5, 5 }, { 6, 5 }, { 6, 6 }, { 8, 5 }, { 8, 6 },
            { 8, 8 }, { 10, 5 }, { 10, 6 }, { 10, 8 }, { 10, 10 }, { 12, 10 }, { 12, 12 },
        };
        uint32_t i = ((uint32_t)f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2;

        bytes = 16;
        bw = dims[i][0];
        bh = dims[i][1];
    }
done:
    if (!bytes)
        return false;
    out->block_bytes = bytes;
    out->block_w = bw;
    out->block_h = bh;
    return true;
}
