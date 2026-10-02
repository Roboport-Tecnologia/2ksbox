/*
 * The zero-copy ring with the frontend's Vulkan import, and nothing else:
 * no guest, no player, no wgpu, no window (doc 12 §4).
 *
 * Why it exists. The ring's second buffer stops being written through: GL
 * hands back every frame blitted into it while the dma-buf's own memory
 * (what the frontend imported and samples) keeps the frame it held first,
 * so one publish in three is a frozen picture. `tools/embed-3d-test.c`
 * shows the backend alone writing through to every slot, so it takes the
 * import to go wrong; but that test has no Vulkan in it and the player has
 * a whole guest behind it. This is the middle: the same ring the backend
 * drives, imported exactly the way `player-core/src/dmabuf.rs` imports it, and
 * the buffer's memory read straight back with the CPU after every frame.
 *
 * It is also the thing to bisect. `--stage` picks how much of the import to
 * do, so the parameter that matters can be found by running it a few times
 * rather than argued about, and if the full stage reproduces here, this is
 * the reproducer to carry anywhere else.
 *
 * Linux only (EGL backend). Build & run from the repo root:
 *   cc -O1 -std=gnu11 -Iembed -o build/zc-vulkan-test tools/zc-vulkan-test.c \
 *      -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,$PWD/build/qemu \
 *      -lepoxy -lvulkan && build/zc-vulkan-test
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <epoxy/gl.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include "libqemu_embed.h"

int InitMesaGL(void);
void FiniMesaGL(void);
void MGLTmpContext(void);
int MGLChoosePixelFormat(void);
int MGLSetPixelFormat(int, const void *);
int MGLCreateContext(uint32_t);
int MGLMakeCurrent(uint32_t, int);
int MGLSwapBuffers(void);
void MGLDeleteContext(int);
void MGLWndRelease(void);

#define MESAGL_MAGIC 0x5b5eb5e5
#define MAXSLOT 8

/* How much of the frontend's import to perform. */
enum {
    ST_NONE,        /* no Vulkan at all: the control, i.e. embed-3d        */
    ST_MEM,         /* import the memory, no image                         */
    ST_IMAGE,       /* create the image, import nothing                    */
    ST_NODEDICATED, /* the full import without the dedicated allocation    */
    ST_LINEAR,      /* ... with VK_IMAGE_TILING_LINEAR, no modifier struct */
    ST_FULL,        /* exactly what player-core/src/dmabuf.rs does              */
};
static const char *const stage_name[] = {
    "none", "mem", "image", "nodedicated", "linear", "full",
};
static int stage = ST_FULL;

static VkInstance inst;
static VkPhysicalDevice phd;
static VkDevice dev;
static PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkBuffer stage_buf;
static VkDeviceMemory stage_mem;
static void *stage_ptr;
static uint32_t qfam;
/* `--use=copy`: read every published frame back through Vulkan as well, the
 * way the frontend samples it. Importing a buffer is not using it, and the
 * frontend does both. */
static int use_copy;

/*
 * `--threaded` does every Vulkan call on a thread of its own, which is
 * where the frontend does them: the blits are on QEMU's vCPU thread and the
 * import and the sampling are on the render thread, and accepting an offer
 * only queues it. Serialised on one thread the ring behaves; this is the
 * last structural difference left between this test and the player.
 */
static int threaded;
/* `--draw=scene`: render like a game instead of clearing (see main) */
static int draw_scene;
static pthread_t worker;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;
static struct { int slot, fd, w, h; uint32_t stride; uint64_t modifier; } pending[MAXSLOT];
static int npending, ready_slot = -1, worker_stop;

static struct {
    int w, h;
    uint32_t stride;
    void *map;
    size_t len;
    VkImage image;
    VkDeviceMemory mem;
    int imported;
    /* what the buffer's own memory held first, and whether it ever moved */
    uint32_t first;
    int n, changed;
    /* the same, as Vulkan reads the image it imported */
    uint32_t vk_first;
    int vk_n, vk_changed, vk_layout;
} slots[MAXSLOT];
static int dmabufs, readies;

static int vk_init(void)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "zc-vulkan-test",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n");
        return 0;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n ? n : 1, sizeof *pds);
    vkEnumeratePhysicalDevices(inst, &n, pds);
    static const char *const want[] = {
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
    };
    for (uint32_t i = 0; i < n && !phd; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            continue;   /* the GL side is the real GPU; match it */
        }
        uint32_t m = 0;
        vkEnumerateDeviceExtensionProperties(pds[i], NULL, &m, NULL);
        VkExtensionProperties *ex = calloc(m ? m : 1, sizeof *ex);
        vkEnumerateDeviceExtensionProperties(pds[i], NULL, &m, ex);
        int have = 0;
        for (int w = 0; w < 3; w++) {
            for (uint32_t j = 0; j < m; j++) {
                if (!strcmp(ex[j].extensionName, want[w])) { have++; break; }
            }
        }
        free(ex);
        if (have == 3) {
            phd = pds[i];
            printf("Vulkan device: %s\n", p.deviceName);
        }
    }
    free(pds);
    if (!phd) {
        printf("no Vulkan device with the dma-buf import extensions\n");
        return 0;
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phd, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phd, &nq, qf);
    qfam = 0;
    for (uint32_t i = 0; i < nq; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    }
    free(qf);
    float pri = 1.f;
    VkDeviceQueueCreateInfo q = {
        .queueFamilyIndex = qfam,
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &pri,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q,
        .enabledExtensionCount = 3,
        .ppEnabledExtensionNames = want,
    };
    if (vkCreateDevice(phd, &dci, NULL, &dev) != VK_SUCCESS) {
        printf("vkCreateDevice failed\n");
        return 0;
    }
    getMemoryFdProperties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    if (!getMemoryFdProperties) {
        return 0;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qfam,
    };
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS) {
        return 0;
    }
    cbi.commandPool = pool;
    return vkAllocateCommandBuffers(dev, &cbi, &cmd) == VK_SUCCESS
        && vkCreateFence(dev, &fci, NULL, &fence) == VK_SUCCESS;
}

/* A host-visible buffer to copy a frame into, made once. */
static int vk_stage(VkDeviceSize size)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, NULL, &stage_buf) != VK_SUCCESS) {
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, stage_buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phd, &mp);
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i))
            && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = (int)i;
        }
    }
    if (type < 0) {
        return 0;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    return vkAllocateMemory(dev, &mai, NULL, &stage_mem) == VK_SUCCESS
        && vkBindBufferMemory(dev, stage_buf, stage_mem, 0) == VK_SUCCESS
        && vkMapMemory(dev, stage_mem, 0, VK_WHOLE_SIZE, 0, &stage_ptr) == VK_SUCCESS;
}

/*
 * Read the slot back through Vulkan, as the frontend's render pass reaches
 * it: the first use transitions the image out of UNDEFINED, which is where
 * wgpu's own tracker starts it, and every later one from the layout it was
 * left in. Returns the same pixel the CPU map is checked at.
 */
static uint32_t vk_read(int slot)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    /* `--use=shader` puts the image through the layout wgpu leaves a sampled
     * texture in, which is the one the frontend's render pass uses; the
     * plain copy path never asks for it. */
    VkImageLayout rest = use_copy == 2 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = slots[slot].vk_layout ? rest : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slots[slot].image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { slots[slot].w, slots[slot].h, 1 },
    };
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    vkCmdCopyImageToBuffer(cmd, slots[slot].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stage_buf, 1, &region);
    if (rest != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.newLayout = rest;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    }
    vkEndCommandBuffer(cmd);
    slots[slot].vk_layout = 1;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    const uint8_t *p = stage_ptr;
    return *(const uint32_t *)(p + (size_t)(slots[slot].h / 2) * slots[slot].w * 4
                               + (slots[slot].w / 2) * 4);
}

/* The frontend's import, as player-core/src/dmabuf.rs performs it. */
static int vk_import(int slot, int fd, int w, int h, uint32_t stride, uint64_t modifier)
{
    VkExternalMemoryImageCreateInfo extmem = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkSubresourceLayout layout = { .offset = 0, .size = 0, .rowPitch = stride };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modinfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = &extmem,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &layout,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = (stage == ST_LINEAR) ? (void *)&extmem : (void *)&modinfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = (stage == ST_LINEAR) ? VK_IMAGE_TILING_LINEAR
                                       : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    VkDeviceSize size = (VkDeviceSize)stride * h;
    uint32_t type_bits = ~0u;

    if (stage != ST_MEM) {
        if (vkCreateImage(dev, &ici, NULL, &image) != VK_SUCCESS) {
            printf("slot %d: vkCreateImage failed\n", slot);
            return 0;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, image, &req);
        size = req.size;
        type_bits = req.memoryTypeBits;
        slots[slot].image = image;
    }
    if (stage == ST_IMAGE) {
        return 1;   /* an image over nothing: no memory imported */
    }

    VkMemoryFdPropertiesKHR fdp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (getMemoryFdProperties(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp)
        != VK_SUCCESS) {
        printf("slot %d: vkGetMemoryFdProperties failed\n", slot);
        return 0;
    }
    type_bits &= fdp.memoryTypeBits;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phd, &mp);
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; i++) {
        if (type_bits & (1u << i)) {
            type = (int)i;
        }
    }
    if (type < 0) {
        printf("slot %d: no memory type accepts the dma-buf\n", slot);
        return 0;
    }
    VkImportMemoryFdInfoKHR imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &imp,
        .image = image,
    };
    int want_ded = (stage == ST_FULL || stage == ST_LINEAR) && image;
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = want_ded ? (void *)&ded : (void *)&imp,
        .allocationSize = size,
        .memoryTypeIndex = (uint32_t)type,
    };
    if (vkAllocateMemory(dev, &mai, NULL, &slots[slot].mem) != VK_SUCCESS) {
        printf("slot %d: vkAllocateMemory(import) failed\n", slot);
        return 0;
    }
    if (image && vkBindImageMemory(dev, image, slots[slot].mem, 0) != VK_SUCCESS) {
        printf("slot %d: vkBindImageMemory failed\n", slot);
        return 0;
    }
    return 1;
}

static void vk_record(int slot, uint32_t v)
{
    if (slots[slot].vk_n++ == 0) {
        slots[slot].vk_first = v;
    } else if (v != slots[slot].vk_first) {
        slots[slot].vk_changed = 1;
    }
}

/* The frontend's thread: import what has been offered, sample what is
 * ready, and nothing else touches Vulkan. */
static void *worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct { int slot, fd, w, h; uint32_t stride; uint64_t modifier; } todo[MAXSLOT];
        int n = 0, slot;

        pthread_mutex_lock(&mtx);
        while (!npending && ready_slot < 0 && !worker_stop) {
            pthread_cond_wait(&cnd, &mtx);
        }
        if (worker_stop && !npending && ready_slot < 0) {
            pthread_mutex_unlock(&mtx);
            return NULL;
        }
        n = npending;
        memcpy(todo, pending, sizeof(pending[0]) * (size_t)n);
        npending = 0;
        slot = ready_slot;
        ready_slot = -1;
        pthread_mutex_unlock(&mtx);

        for (int i = 0; i < n; i++) {
            int ok = vk_import(todo[i].slot, todo[i].fd, todo[i].w, todo[i].h,
                               todo[i].stride, todo[i].modifier);
            slots[todo[i].slot].imported = ok;
            if (!ok) {
                close(todo[i].fd);
            }
            if (use_copy && !stage_buf
                && !vk_stage((VkDeviceSize)todo[i].w * todo[i].h * 4)) {
                printf("staging buffer failed; --use off\n");
                use_copy = 0;
            }
            printf("slot %d: imported on the frontend's thread -> %s\n", todo[i].slot,
                   ok ? "ok" : "FAILED");
        }
        if (slot >= 0 && use_copy && slots[slot].image && slots[slot].imported) {
            vk_record(slot, vk_read(slot));
        }
    }
}

static int on_3d_dmabuf(void *ud, int slot, int fd, int w, int h, int stride,
                        uint32_t fourcc, uint64_t modifier)
{
    if (slot < 0 || slot >= MAXSLOT) {
        close(fd);
        return 0;
    }
    /* One description for the CPU check, one for Vulkan: importing passes
     * ownership, so the two consumers cannot share the fd we were given. */
    int mapfd = dup(fd);
    slots[slot].w = w;
    slots[slot].h = h;
    slots[slot].stride = (uint32_t)stride;
    slots[slot].len = (size_t)stride * h;
    slots[slot].map = mmap(NULL, slots[slot].len, PROT_READ, MAP_SHARED, mapfd, 0);
    if (slots[slot].map == MAP_FAILED) {
        printf("slot %d: mmap failed\n", slot);
        slots[slot].map = NULL;
        close(mapfd);
        close(fd);
        return 0;
    }
    close(mapfd);
    if (stage == ST_NONE) {
        close(fd);
    } else if (threaded) {
        /* accepting only queues it, as the frontend's does */
        pthread_mutex_lock(&mtx);
        pending[npending++] = (typeof(pending[0])){ slot, fd, w, h, (uint32_t)stride, modifier };
        pthread_cond_signal(&cnd);
        pthread_mutex_unlock(&mtx);
    } else {
        slots[slot].imported = vk_import(slot, fd, w, h, (uint32_t)stride, modifier);
        if (!slots[slot].imported) {
            close(fd);
        }
        /* on success the fd belongs to Vulkan (or to nothing, for ST_IMAGE) */
        if (stage == ST_IMAGE) {
            close(fd);
        }
    }
    if (use_copy && !stage_buf && slots[slot].imported && !vk_stage((VkDeviceSize)w * h * 4)) {
        printf("staging buffer failed; --use=copy off\n");
        use_copy = 0;
    }
    dmabufs++;
    printf("slot %d: %dx%d stride %d modifier 0x%llx fourcc %c%c%c%c -> %s\n",
           slot, w, h, stride, (unsigned long long)modifier,
           fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff,
           stage == ST_NONE ? "no import"
           : threaded ? "queued for the frontend's thread"
           : (slots[slot].imported ? "imported" : "IMPORT FAILED"));
    return 1;
}

/* The buffer's own memory, after the blit the backend has just finished. */
static void on_3d_frame_ready(void *ud, int slot)
{
    readies++;
    if (slot < 0 || slot >= MAXSLOT || !slots[slot].map) {
        return;
    }
    const uint8_t *p = slots[slot].map;
    uint32_t c = *(const uint32_t *)(p + (size_t)(slots[slot].h / 2) * slots[slot].stride
                                     + (slots[slot].w / 2) * 4);
    if (slots[slot].n++ == 0) {
        slots[slot].first = c;
    } else if (c != slots[slot].first) {
        slots[slot].changed = 1;
    }
    if (!use_copy) {
        return;
    }
    if (threaded) {
        /* the newest frame wins, as take_if_newer does */
        pthread_mutex_lock(&mtx);
        ready_slot = slot;
        pthread_cond_signal(&cnd);
        pthread_mutex_unlock(&mtx);
    } else if (slots[slot].image && slots[slot].imported) {
        vk_record(slot, vk_read(slot));
    }
}

static void on_3d_active(void *ud, bool on) { (void)ud; (void)on; }

int main(int argc, char **argv)
{
    const char *bios = "qemu/pc-bios";
    int frames = 60;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--stage=", 8)) {
            const char *s = argv[i] + 8;
            stage = -1;
            for (int k = 0; k <= ST_FULL; k++) {
                if (!strcmp(s, stage_name[k])) { stage = k; }
            }
            if (stage < 0) {
                printf("stages: none mem image nodedicated linear full\n");
                return 2;
            }
        } else if (!strncmp(argv[i], "--use=", 6)) {
            use_copy = !strcmp(argv[i] + 6, "copy") ? 1
                     : !strcmp(argv[i] + 6, "shader") ? 2 : 0;
        } else if (!strncmp(argv[i], "--draw=", 7)) {
            draw_scene = !strcmp(argv[i] + 7, "scene") ? 1
                       : !strcmp(argv[i] + 7, "front") ? 2
                       : !strcmp(argv[i] + 7, "all") ? 3
                       : !strcmp(argv[i] + 7, "alloc") ? 4
                       : !strcmp(argv[i] + 7, "load") ? 5 : 0;
        } else if (!strcmp(argv[i], "--threaded")) {
            threaded = 1;
        } else if (!strncmp(argv[i], "--frames=", 9)) {
            frames = atoi(argv[i] + 9);
        } else if (!strncmp(argv[i], "--bios=", 7)) {
            bios = argv[i] + 7;
        } else {
            printf("usage: %s [--stage=NAME] [--use=none|copy|shader] [--draw=clear|scene|front|all|alloc|load] [--threaded] [--frames=N] [--bios=DIR]\n",
                   argv[0]);
            return 2;
        }
    }
    /* the ring is stood down by default (doc 12 §4); this test is about it */
    if (!getenv("EMBED_ZC_SLOTS")) {
        setenv("EMBED_ZC_SLOTS", "3", 1);
    }
    printf("stage %s, use %s, %s, draw %s, %d frames, ring %s\n", stage_name[stage],
           use_copy == 2 ? "shader" : use_copy ? "copy" : "none",
           threaded ? "vulkan on its own thread" : "one thread",
           draw_scene == 5 ? "load" : draw_scene == 4 ? "alloc" : draw_scene == 3 ? "all" : draw_scene == 2 ? "front" : draw_scene ? "scene" : "clear", frames, getenv("EMBED_ZC_SLOTS"));

    if (stage != ST_NONE && !vk_init()) {
        return 1;
    }
    if (threaded && stage != ST_NONE && pthread_create(&worker, NULL, worker_main, NULL)) {
        printf("pthread_create failed\n");
        return 1;
    }

    char *qargv[] = {
        "qemu-system-i386", "-machine", "pc", "-m", "32", "-net", "none",
        "-L", (char *)bios, "-nodefaults", "-vga", "std",
    };
    qemu_embed_display_cb cb = {
        .on_3d_active = on_3d_active,
        .on_3d_dmabuf = on_3d_dmabuf,
        .on_3d_frame_ready = on_3d_frame_ready,
    };
    if (qemu_embed_api_version() != QEMU_EMBED_API_VERSION) {
        printf("API version mismatch\n");
        return 1;
    }
    if (!qemu_embed_new(sizeof(qargv) / sizeof(qargv[0]), qargv, &cb, NULL)) {
        printf("qemu_embed_new failed\n");
        return 1;
    }
    if (InitMesaGL() != 0) {
        printf("InitMesaGL failed\n");
        return 1;
    }
    MGLTmpContext();
    uint8_t pfd[64] = {0};
    MGLSetPixelFormat(MGLChoosePixelFormat(), pfd);
    if (MGLCreateContext(MESAGL_MAGIC)) {
        printf("MGLCreateContext failed\n");
        return 1;
    }
    MGLMakeCurrent(MESAGL_MAGIC, 0);
    printf("GL %s\n", (const char *)glGetString(GL_VERSION));

    /*
     * `--draw=scene` renders the way a game does rather than clearing: a
     * depth-tested textured quad, the texture reuploaded every frame. A bare
     * glClear is not what the guest does, and which of the two the producer
     * is running turns out to decide whether a slot keeps being written
     * through (GLQuake diverges, wglgears does not; doc 12 §4).
     */
    GLuint tex = 0;
    static uint32_t texels[64 * 64];
    if (draw_scene) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_DEPTH_TEST);
    }
    for (int i = 0; i < frames; i++) {
        glClearColor(0.f, (float)(i % 16) / 16.f, (float)(i % 5) / 5.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | (draw_scene ? GL_DEPTH_BUFFER_BIT : 0));
        if (draw_scene == 2) {
            /* GLQuake's front-buffer path (the loading plaque and the disc
             * icon): draw into GL_FRONT, flush, and go back to GL_BACK. It
             * is the call the trace has that wglgears' has not. */
            glDrawBuffer(GL_FRONT);
            glBegin(GL_QUADS);
            glVertex3f(-.2f, -.2f, 0.f); glVertex3f(.2f, -.2f, 0.f);
            glVertex3f(.2f, .2f, 0.f);   glVertex3f(-.2f, .2f, 0.f);
            glEnd();
            glFlush();
            glDrawBuffer(GL_BACK);
        }
        if (draw_scene == 5 && i == 20) {
            /*
             * The frame GLQuake breaks on, by its measured shape: a level
             * load, which uploads ~934 textures and toggles the draw buffer
             * to the front and back ~165 times in the one frame (the disc
             * icon it flashes between loads). Neither volume appears in any
             * frame before it, and that is the frame a slot stops aliasing.
             */
            static GLuint many[934];
            uint32_t *px = calloc(64 * 64, 4);
            glGenTextures(934, many);
            for (int t = 0; t < 934; t++) {
                if ((t % 6) == 0) {
                    glDrawBuffer(GL_FRONT);
                    glBegin(GL_QUADS);
                    glVertex2f(.8f, .8f); glVertex2f(.9f, .8f);
                    glVertex2f(.9f, .9f); glVertex2f(.8f, .9f);
                    glEnd();
                    glFlush();
                    glDrawBuffer(GL_BACK);
                }
                glBindTexture(GL_TEXTURE_2D, many[t]);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_BGRA,
                             GL_UNSIGNED_BYTE, px);
            }
            free(px);
            printf("level load: 934 textures, %d front/back toggles\n", 934 / 6);
            fflush(stdout);
            glBindTexture(GL_TEXTURE_2D, tex);
        }
        if (draw_scene == 4 && i == 20) {
            /*
             * A level load: a burst of texture allocation, which is what a
             * game does a dozen frames in and neither wglgears nor the
             * scene above does at all. If a slot stops aliasing its buffer
             * here, the trigger is the driver moving memory under pressure,
             * not any particular call.
             */
            static GLuint big[256];
            static uint32_t *px;
            px = calloc(512 * 512, 4);
            glGenTextures(256, big);
            for (int t = 0; t < 256; t++) {
                glBindTexture(GL_TEXTURE_2D, big[t]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_BGRA,
                             GL_UNSIGNED_BYTE, px);
            }
            free(px);
            printf("uploaded 256 x 512x512 textures (256 MB)\n");
            fflush(stdout);
            glBindTexture(GL_TEXTURE_2D, tex);
        }
        if (draw_scene == 3 || draw_scene == 4 || draw_scene == 5) {
            /*
             * Every call the trace has for GLQuake and not for wglgears, in
             * a frame shaped like one of its own: matrices, depth, culling,
             * a texture rebuilt and sub-loaded each frame, alpha test,
             * blending, immediate-mode colours and vertices, the gets, and
             * the front-buffer pass. If the ring survives this, the cause
             * is not in the set of calls and bisecting them is pointless.
             */
            glViewport(0, 0, slots[0].w ? slots[0].w : 640,
                       slots[0].h ? slots[0].h : 480);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glFrustum(-1, 1, -1, 1, 1, 4096);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glRotatef((float)i, 0, 0, 1);
            glTranslatef(0, 0, -2.f);
            GLfloat m[16];
            glGetFloatv(GL_MODELVIEW_MATRIX, m);
            (void)glGetString(GL_EXTENSIONS);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            glDepthRange(0, 1);
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.666f);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glBindTexture(GL_TEXTURE_2D, tex);
            for (int k = 0; k < 64 * 64; k++) {
                texels[k] = 0xff000000u | (uint32_t)((k + i) & 0xff) << 8;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_BGRA,
                         GL_UNSIGNED_BYTE, texels);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_BGRA,
                            GL_UNSIGNED_BYTE, texels);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            static const GLubyte c3[3] = { 200, 180, 160 };
            static const GLfloat c4[4] = { .8f, .7f, .6f, 1.f };
            glColor3f(1.f, 1.f, 1.f);
            glColor3ubv(c3);
            glColor4f(.9f, .8f, .7f, 1.f);
            glColor4fv(c4);
            float z = (float)(i % 8) / 16.f;
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(-.9f, -.9f, z);
            glTexCoord2f(1, 0); glVertex3f(.9f, -.9f, z);
            glTexCoord2f(1, 1);
            GLfloat v[3] = { .9f, .9f, z };
            glVertex3fv(v);
            glTexCoord2f(0, 1); glVertex3f(-.9f, .9f, z);
            glEnd();
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0, 1, 1, 0, -99999, 99999);
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glScalef(.5f, .5f, 1.f);
            glBegin(GL_QUADS);
            glVertex2f(.1f, .1f); glVertex2f(.4f, .1f);
            glVertex2f(.4f, .4f); glVertex2f(.1f, .4f);
            glEnd();
            glPopMatrix();
            glDisable(GL_BLEND);
            glDisable(GL_ALPHA_TEST);
            glDrawBuffer(GL_FRONT);
            glBegin(GL_QUADS);
            glVertex2f(.8f, .8f); glVertex2f(.9f, .8f);
            glVertex2f(.9f, .9f); glVertex2f(.8f, .9f);
            glEnd();
            glFlush();
            glDrawBuffer(GL_BACK);
            (void)m;
        }
        if (draw_scene == 1) {
            for (int k = 0; k < 64 * 64; k++) {
                texels[k] = 0xff000000u | (uint32_t)((k + i) & 0xff) << 8;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_BGRA,
                         GL_UNSIGNED_BYTE, texels);
            float z = (float)(i % 8) / 16.f;
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(-.9f, -.9f, z);
            glTexCoord2f(1, 0); glVertex3f(.9f, -.9f, z);
            glTexCoord2f(1, 1); glVertex3f(.9f, .9f, z);
            glTexCoord2f(0, 1); glVertex3f(-.9f, .9f, z);
            glEnd();
        }
        MGLSwapBuffers();
    }

    if (threaded && stage != ST_NONE) {
        pthread_mutex_lock(&mtx);
        worker_stop = 1;
        pthread_cond_signal(&cnd);
        pthread_mutex_unlock(&mtx);
        pthread_join(worker, NULL);
    }

    int bad = 0;
    for (int i = 0; i < MAXSLOT; i++) {
        if (slots[i].n < 2) {
            continue;
        }
        printf("slot %d: %d frames, memory %s (first %08x)", i, slots[i].n,
               slots[i].changed ? "follows the blits" : "NEVER CHANGED", slots[i].first);
        if (slots[i].vk_n >= 2) {
            printf(", vulkan %s (first %08x)",
                   slots[i].vk_changed ? "follows" : "NEVER CHANGED", slots[i].vk_first);
        }
        printf("\n");
        bad += !slots[i].changed || (slots[i].vk_n >= 2 && !slots[i].vk_changed);
    }
    printf("stage %s: %d dmabufs, %d frames -> %s\n", stage_name[stage], dmabufs, readies,
           bad ? "REPRODUCED (a slot froze)" : "clean");
    fflush(stdout);
    _exit(bad ? 1 : 0);   /* never exit() with the QEMU thread alive */
}
