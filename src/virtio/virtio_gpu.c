#include "sov_hypervisor.h"

// =============================================================================
// VirtIO-GPU — Adreno Passthrough
// =============================================================================
// Virtual VirtIO-GPU PCI device that captures guest rendering commands
// and translates them to host Vulkan calls on Snapdragon Adreno GPU.
// =============================================================================

#define VIRTIO_GPU_BAR_BASE     0xFE000000UL
#define VIRTIO_GPU_BAR_SIZE     0x00010000UL  // 64KB MMIO

// VirtIO-GPU command types (subset)
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO     0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF       0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT          0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH       0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_CTX_CREATE           0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY          0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE  0x0202
#define VIRTIO_GPU_CMD_SUBMIT_3D            0x0207

// VirtIO-GPU control header
typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} virtio_gpu_ctrl_hdr_t;

// Resource tracking
#define MAX_GPU_RESOURCES 1024

typedef struct {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t host_handle;   // Vulkan image/buffer handle
    uint64_t backing_addr;  // Guest physical address of backing pages
    uint64_t backing_size;
} gpu_resource_t;

typedef struct {
    gpu_resource_t resources[MAX_GPU_RESOURCES];
    uint32_t       resource_count;
    uint32_t       scanout_resource_id;
    // VirtIO register state
    uint32_t       queue_sel;
    uint32_t       queue_notify;
    uint32_t       device_status;
    uint32_t       isr;
    // Virtqueue descriptors
    uint64_t       vq_desc_addr;
    uint64_t       vq_avail_addr;
    uint64_t       vq_used_addr;
    uint16_t       vq_size;
} virtio_gpu_state_t;

static virtio_gpu_state_t gpu_state;

// =============================================================================
// MMIO Handlers (called from trap loop when guest touches GPU BAR)
// =============================================================================

static int virtio_gpu_mmio_read(uint64_t offset, uint32_t size,
                                uint64_t *value, void *opaque) {
    (void)opaque;
    (void)size;

    switch (offset) {
    case 0x000: *value = 0x74726976; break;  // Magic: "virt"
    case 0x004: *value = 2;          break;  // Version: 2 (MMIO)
    case 0x008: *value = 16;         break;  // DeviceID: GPU
    case 0x00C: *value = 0x554D4551; break;  // VendorID: "QEMU" (compatible)
    case 0x010: *value = 0x1;        break;  // Device features (bit 0: virgl)
    case 0x060: *value = gpu_state.isr; break;
    case 0x070: *value = gpu_state.device_status; break;
    default:    *value = 0;          break;
    }

    return 0;
}

static int virtio_gpu_mmio_write(uint64_t offset, uint32_t size,
                                 uint64_t value, void *opaque) {
    (void)opaque;
    (void)size;

    switch (offset) {
    case 0x014: // Device features sel
        break;
    case 0x020: // Driver features
        break;
    case 0x030: // Queue sel
        gpu_state.queue_sel = (uint32_t)value;
        break;
    case 0x038: // Queue size
        gpu_state.vq_size = (uint16_t)value;
        break;
    case 0x044: // Queue ready
        break;
    case 0x050: // Queue notify
        gpu_state.queue_notify = (uint32_t)value;
        virtio_gpu_process_queue();
        break;
    case 0x064: // Interrupt ACK
        gpu_state.isr &= ~(uint32_t)value;
        break;
    case 0x070: // Device status
        gpu_state.device_status = (uint32_t)value;
        if (value == 0) {
            // Reset
            gpu_state.resource_count = 0;
        }
        break;
    case 0x080: // Queue desc low
        gpu_state.vq_desc_addr = (gpu_state.vq_desc_addr & 0xFFFFFFFF00000000UL) | (uint32_t)value;
        break;
    case 0x084: // Queue desc high
        gpu_state.vq_desc_addr = (gpu_state.vq_desc_addr & 0xFFFFFFFF) | (value << 32);
        break;
    case 0x090: // Queue avail low
        gpu_state.vq_avail_addr = (gpu_state.vq_avail_addr & 0xFFFFFFFF00000000UL) | (uint32_t)value;
        break;
    case 0x094: // Queue avail high
        gpu_state.vq_avail_addr = (gpu_state.vq_avail_addr & 0xFFFFFFFF) | (value << 32);
        break;
    case 0x0A0: // Queue used low
        gpu_state.vq_used_addr = (gpu_state.vq_used_addr & 0xFFFFFFFF00000000UL) | (uint32_t)value;
        break;
    case 0x0A4: // Queue used high
        gpu_state.vq_used_addr = (gpu_state.vq_used_addr & 0xFFFFFFFF) | (value << 32);
        break;
    }

    return 0;
}

// =============================================================================
// Command Processing — Translate GPU commands to Vulkan
// =============================================================================

static void virtio_gpu_process_queue(void) {
    // Walk the virtqueue descriptors and process GPU commands
    // In a real implementation:
    //   1. Read descriptor chain from vq_desc_addr
    //   2. Parse virtio_gpu_ctrl_hdr_t
    //   3. Dispatch to command handlers
    //   4. For SUBMIT_3D: translate guest command buffer → Vulkan calls
    //   5. Post completion to vq_used_addr
    //   6. Signal interrupt (ISR)

    gpu_state.isr |= 1;
}

static void handle_resource_create_2d(uint32_t resource_id,
                                      uint32_t format,
                                      uint32_t width,
                                      uint32_t height) {
    if (gpu_state.resource_count >= MAX_GPU_RESOURCES) return;

    gpu_resource_t *res = &gpu_state.resources[gpu_state.resource_count++];
    res->id = resource_id;
    res->width = width;
    res->height = height;
    res->format = format;
    res->host_handle = 0;  // Vulkan: vkCreateImage() here

    // In production:
    // VkImageCreateInfo ci = { .imageType = VK_IMAGE_TYPE_2D, ... };
    // vkCreateImage(device, &ci, NULL, &res->host_handle);
    // vkAllocateMemory(...);
    // vkBindImageMemory(...);
}

static void handle_submit_3d(uint32_t ctx_id, void *cmd_buf, uint64_t size) {
    // Translate guest 3D command buffer to host Vulkan
    // This is where Adreno-specific optimization happens:
    //   - Parse guest command stream (Gallium/virgl format)
    //   - Convert to Vulkan command buffer
    //   - Submit to Adreno via vkQueueSubmit
    //   - Use VK_KHR_external_memory for zero-copy where possible

    (void)ctx_id;
    (void)cmd_buf;
    (void)size;
}

// =============================================================================
// Initialization
// =============================================================================

int sov_virtio_gpu_init(sov_guest_t *guest) {
    // Reset state
    gpu_state.resource_count = 0;
    gpu_state.device_status = 0;
    gpu_state.isr = 0;
    gpu_state.queue_sel = 0;

    // Register MMIO region so guest accesses trap to our handlers
    return sov_mmio_register(guest,
                            VIRTIO_GPU_BAR_BASE,
                            VIRTIO_GPU_BAR_SIZE,
                            virtio_gpu_mmio_read,
                            virtio_gpu_mmio_write,
                            NULL);
}

int sov_virtio_gpu_submit(sov_guest_t *guest, void *cmd_buf, uint64_t size) {
    (void)guest;
    handle_submit_3d(0, cmd_buf, size);
    return 0;
}
