/* SPDX-License-Identifier: GPL-2.0-or-later
 * Thin Vulkan compute host. No encoding logic — only setup, buffers, dispatch. */
#ifndef VK264_HOST_H
#define VK264_HOST_H

#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool cmd_pool;
} vk_host_t;

int vk_host_init(vk_host_t *h);
void vk_host_shutdown(vk_host_t *h);

/* Single storage-buffer compute shader. Returns 0 on success. */
int vk_host_dispatch(vk_host_t *h, const char *spv_path, void *data, size_t bytes,
                     uint32_t workgroups);

#endif /* VK264_HOST_H */
