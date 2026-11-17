# Lazy NVK start in the Metal plugin (G15 fork, `g15-lazy`)

On a dual-GPU laptop (Dell G15 5530, Intel UHD + RTX 4050 Laptop) every Metal process woke the NVIDIA GPU, even apps
that never use it. Measured with the GPU powered off: a program that only calls `MTLCopyAllDevices()` and reads the
device names turned it back on and created the whole NVK device.

Cause: `-[NVMTLDevice initWithAcceleratorPort:]` (run when Metal lists the devices) calls `nvmtl_vk_init()`, and
`-name` calls it too. On Apple's drivers listing a discrete GPU is free; the GPU only powers up with real work.

Change (plugin only):
- A snapshot of the fixed device properties (`g_name`, `VkPhysicalDeviceLimits`, memory properties, the feature flags,
  working set, timestamp frequency) is written once to `NVMTLDriver.bundle/Contents/Resources/nvmtl-lazy.bin` by a root
  process started with `G15_LAZY_SAVE=1` (a release build drops `NVMTL_*` variables, so the trigger uses another prefix).
  It is checked against the PCI device id and the plugin build date; without a valid snapshot nothing changes.
- With the snapshot, `initWithAcceleratorPort:` does not start Vulkan; `name`, limits, caps, working set, timestamp
  frequency, ray tracing / barycentrics and `supports32Bit*` answer from it.
- Vulkan starts on the first real use (command queue, buffer, texture, pipeline, sampler, heap); those paths already
  called `nvmtl_vk_init()`. WindowServer (and `G15_NVMTL_EAGER=1`) still start it at once.

Result on the G15 with the GPU off: listing, TextEdit, Safari, NotebookLM, WhatsApp do not wake it; Preview (Live Text
runs on the GPU) and games do, and run normally (Hollow Knight). After a logout the Metal connections on the NVIDIA GPU
went from 232 to 64.
