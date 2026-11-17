# Kintex-7 Apex GPU Compared With an Integrated Intel GPU

Both devices are in the same Ubuntu 24.04 host: the Kintex-7 board with the
Apex GPU image and drivers from this repository, and the host processor's
integrated Intel Graphics (Arrow Lake) with Ubuntu's Mesa 25.2.8 driver.
`vulkaninfo` reported the properties, limits and features, and `vkmark`
measured the frame rates.

## Devices

| | Intel Graphics (Arrow Lake) | Apex GPU on the Kintex-7 |
| --- | --- | --- |
| Type | Integrated GPU | Discrete PCIe GPU (FPGA) |
| Vulkan version | 1.4.318 | 1.4.0 |
| Khronos conformance | 1.4.0.0 | Not submitted (0.0.0.0) |
| Driver | Intel open-source Mesa driver, Mesa 25.2.8 | Apex, Mesa 26.3.0-devel (`mesa-kintex7`) |
| PCI ID | `8086:7d67` | `10ee:a15e` (Vulkan vendor `0x10009`) |
| Memory | 34.9 GiB, shared with system memory | 2 GiB of DDR3 on the board |
| Subgroup size | 32 | 32 |
| Device extensions | 185 | 93 (all also on the Intel GPU) |
| Core features (of 55) | 51 | 36 |
| Timestamp period | 52.08 ns | 20 ns |
| Host link | Integrated | PCIe Gen2 x4: 2 GB/s per direction raw, about 1.6 GB/s usable |

## vkmark

`vkmark --winsys xcb -p fifo` at its default 800x600, in a virtual X server.
Both devices present the same way: the CPU copies each finished frame to the
window (`MESA_VK_WSI_DEBUG=sw`). The score is the average frame rate.

| Scene | Intel (fps) | Kintex-7 (fps) |
| --- | ---: | ---: |
| vertex, device-local | 171 | 4 |
| vertex, host memory | 171 | 4 |
| texture | 172 | 5 |
| texture, 16x anisotropy | 171 | 4 |
| shading, Gouraud | 170 | 4 |
| shading, Blinn-Phong | 171 | 4 |
| shading, Phong | 170 | 3 |
| shading, cel | 170 | 3 |
| effect2d, edge | 170 | 3 |
| effect2d, blur | 167 | 1 |
| desktop | 168 | 4 |
| cube | 173 | 6 |
| clear | 173 | 2 |
| **Score** | **170** | **3** |

The Intel GPU runs every scene, even `clear`, at the same rate: the frame copy
to the window limits it, so its rendering speed is far above 170 fps. The
Kintex-7 is limited by its single 50 MHz shader core as well as the copy. Every
scene renders correctly on it.

## Limits

| Limit | Intel | Kintex-7 |
| --- | ---: | ---: |
| `maxImageDimension2D` | 16384 | 8192 |
| `maxImageDimension3D` | 2048 | 512 |
| `maxImageArrayLayers` | 2048 | 2048 |
| `maxFramebufferWidth` | 16384 | 8192 |
| `maxColorAttachments` | 8 | 8 |
| `maxBoundDescriptorSets` | 8 | 32 |
| `maxPerStageDescriptorSamplers` | 67108864 | 640 |
| `maxPerStageDescriptorStorageBuffers` | 33554432 | 256 |
| `maxDescriptorSetSampledImages` | 33554432 | 2048 |
| `maxVertexInputAttributes` | 29 | 32 |
| `maxComputeSharedMemorySize` | 65536 | 32768 |
| `maxComputeWorkGroupInvocations` | 1024 | 256 |
| `maxComputeWorkGroupCount` | 65535 x 3 | 65535 x 3 |
| `maxViewports` | 16 | 16 |
| `maxSamplerAnisotropy` | 16 | 16 |
| Color sample counts | 1, 2, 4, 8, 16 | 1, 2, 4, 8 |
| `maxMemoryAllocationCount` | 4294967295 | 4096 |
| `maxPushConstantsSize` | 256 | 256 |
| `maxUniformBufferRange` | 1 GiB | 64 MiB |
| `maxStorageBufferRange` | 4 GiB | 1 GiB |
| `maxClipDistances` | 8 | 8 |
| `pointSizeRange` | 0.125 – 255.875 | 1 – 255.875 |
| `lineWidthRange` | 0 – 8 | 1 – 16 |

## Extensions

Extensions the Intel GPU adds include ray tracing (`VK_KHR_ray_tracing_pipeline`,
`VK_KHR_ray_query`, `VK_KHR_acceleration_structure`), mesh shaders
(`VK_EXT_mesh_shader`), fragment shading rate, graphics pipeline libraries,
descriptor buffers, extended dynamic state 3, 64-bit and float atomics, and
host image copy.

## Reproduce

```bash
# Intel GPU
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.json MESA_VK_WSI_DEBUG=sw \
  xvfb-run -a vkmark --winsys xcb -p fifo

# Kintex-7
VK_DRIVER_FILES=$PWD/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json MESA_VK_WSI_DEBUG=sw \
  xvfb-run -a vkmark --winsys xcb -p fifo

# Properties, limits and features of both
VK_ADD_DRIVER_FILES=$PWD/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json vulkaninfo --summary
```
