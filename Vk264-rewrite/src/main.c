/* SPDX-License-Identifier: GPL-2.0-or-later
 * Correctness-first C + Vulkan x264 port. Milestone 0: build + bitstream. */
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("vk264-rewrite 0.1 (C + Vulkan, correctness-first)\n");
    printf("shader dir: %s\n", VK264_SHADER_DIR);
    return 0;
}
