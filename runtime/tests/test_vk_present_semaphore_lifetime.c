/* Execute the production submit and image-owned resource functions against
 * Vulkan's acquire/wait contract. Completing a frame fence deliberately does
 * not consume a presentation wait. No Vulkan device or window is needed. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "video_telemetry.h"
#include "present_declarations.inc"

#define HANDLE(type, value) ((type)(uintptr_t)(value))
#define TRACKED 2048
static int failures, reuse_errors, next_handle = 100;
static int live[TRACKED], owner[TRACKED], acquired[VK_FRAMES];
static int create_calls, fail_create_at = -1;
static VkSemaphore submitted;
static VkDevice s_dev;
static VkQueue s_queue;
static VkSwapchainKHR s_swapchain;
static VkSemaphore s_sem_acquire[VK_FRAMES];
static VkFence s_fence[VK_FRAMES];
static VkExtent2D s_sc_extent = {640, 480};
static uint32_t s_sc_count, s_frame_idx;
static int s_scale = 1;
static struct { uint64_t present_us; } s_perf_cur;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (0)

static int index_of(VkSemaphore semaphore) {
    unsigned index = (unsigned)(uintptr_t)semaphore;
    CHECK(index < TRACKED);
    return index < TRACKED ? (int)index : 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(
    VkDevice device, const VkSemaphoreCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkSemaphore* out) {
    (void)device; (void)allocator;
    CHECK(info->sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    if (create_calls++ == fail_create_at) {
        /* Failure output is not a valid handle and must never be published. */
        *out = HANDLE(VkSemaphore, TRACKED - 1);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    CHECK(next_handle < TRACKED - 1);
    live[next_handle] = 1;
    owner[next_handle] = -1;
    *out = HANDLE(VkSemaphore, next_handle++);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_semaphore(
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator) {
    (void)device; (void)allocator;
    int index = index_of(semaphore);
    CHECK(live[index]);
    CHECK(owner[index] == -1);
    live[index] = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL end_commands(VkCommandBuffer buffer) {
    (void)buffer;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL queue_submit(
    VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    (void)queue;
    CHECK(count == 1 && submits->waitSemaphoreCount == 1);
    CHECK(submits->signalSemaphoreCount == 1 && submits->commandBufferCount == 1);
    int frame = -1;
    for (int i = 0; i < VK_FRAMES; ++i)
        if (submits->pWaitSemaphores[0] == s_sem_acquire[i]) frame = i;
    CHECK(frame >= 0);
    if (frame < 0) return VK_ERROR_UNKNOWN;
    CHECK(fence == s_fence[frame]);
    CHECK((*submits->pWaitDstStageMask & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0);
    /* Waiting on this acquire retires the preceding present of THIS image,
     * independently of the frame fence or the order of image acquisition. */
    for (int i = 0; i < TRACKED; ++i)
        if (owner[i] == acquired[frame]) owner[i] = -1;
    submitted = submits->pSignalSemaphores[0];
    int index = index_of(submitted);
    CHECK(live[index]);
    if (owner[index] != -1) {
        fprintf(stderr, "present wait reused: frame=%d image=%d pending_image=%d\n",
                frame, acquired[frame], owner[index]);
        ++reuse_errors;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
    (void)queue;
    CHECK(info->waitSemaphoreCount == 1 && info->swapchainCount == 1);
    CHECK(info->pWaitSemaphores[0] == submitted);
    CHECK(info->pSwapchains[0] == s_swapchain);
    CHECK(info->pImageIndices[0] < s_sc_count);
    owner[index_of(submitted)] = (int)info->pImageIndices[0];
    return VK_SUCCESS;
}

static PFN_vkCreateSemaphore p_vkCreateSemaphore = create_semaphore;
static PFN_vkDestroySemaphore p_vkDestroySemaphore = destroy_semaphore;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer = end_commands;
static PFN_vkQueueSubmit p_vkQueueSubmit = queue_submit;
static PFN_vkQueuePresentKHR p_vkQueuePresentKHR = queue_present;
static uint64_t SDL_GetPerformanceCounter(void) { return 1; }
static uint64_t perf_elapsed_us(uint64_t start) { (void)start; return 0; }
static int vk_die(const char* message) { (void)message; return 0; }
void psx_video_telemetry_compose(int a, int b, int c, int d, int e, int f, int g, int h) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
}
void psx_video_telemetry_present(int result) { CHECK(result); }
#include "present_functions.inc"

static void schedule(const uint32_t* images, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t frame = s_frame_idx % VK_FRAMES;
        /* The preceding frame submission is complete, but its presentation
         * wait remains pending until the corresponding image is acquired. */
        acquired[frame] = (int)images[i];
        submit_present(HANDLE(VkCommandBuffer, frame + 1), images[i], frame);
    }
}

static void retire_presentations(void) {
    /* Resource cleanup is tested after the caller has retired presentation;
     * this fixture does not claim that a submit fence provides that proof. */
    for (int i = 0; i < TRACKED; ++i) owner[i] = -1;
}

int main(void) {
    memset(owner, -1, sizeof owner);
    s_dev = HANDLE(VkDevice, 1); s_queue = HANDLE(VkQueue, 2);
    s_swapchain = HANDLE(VkSwapchainKHR, 3);
    for (int i = 0; i < VK_FRAMES; ++i) {
        s_sem_acquire[i] = HANDLE(VkSemaphore, 10 + i);
        s_fence[i] = HANDLE(VkFence, 20 + i);
    }
#if HAVE_IMAGE_SEMAPHORES
    s_sc_count = 0;
    CHECK(!create_present_semaphores());
    s_sc_count = (uint32_t)(sizeof(PRESENT_ARRAY) / sizeof(PRESENT_ARRAY[0])) + 1;
    CHECK(!create_present_semaphores());
    s_sc_count = 3; create_calls = 0; fail_create_at = 2;
    CHECK(!create_present_semaphores());
    for (size_t i = 0; i < sizeof(PRESENT_ARRAY) / sizeof(PRESENT_ARRAY[0]); ++i)
        CHECK(PRESENT_ARRAY[i] == VK_NULL_HANDLE);
    for (int i = 100; i < next_handle; ++i) CHECK(!live[i]);
    fail_create_at = -1;
    CHECK(create_present_semaphores());
#else
    s_sc_count = 3;
    for (size_t i = 0; i < sizeof(PRESENT_ARRAY) / sizeof(PRESENT_ARRAY[0]); ++i) {
        PRESENT_ARRAY[i] = HANDLE(VkSemaphore, next_handle);
        live[next_handle++] = 1;
    }
#endif
    const uint32_t first[] = {0, 1, 2, 0, 2, 1, 0, 1, 2};
    schedule(first, sizeof first / sizeof first[0]);
    CHECK(s_frame_idx == sizeof first / sizeof first[0]);
#if HAVE_IMAGE_SEMAPHORES
    retire_presentations(); destroy_present_semaphores(); destroy_present_semaphores();
    s_sc_count = 4;
    CHECK(create_present_semaphores());
    const uint32_t recreated[] = {3, 0, 2, 1, 3, 1, 0, 2};
    schedule(recreated, sizeof recreated / sizeof recreated[0]);
    retire_presentations(); destroy_present_semaphores();
    for (int i = 100; i < next_handle; ++i) CHECK(!live[i]);
#endif
    CHECK(reuse_errors == 0);
    if (failures) return 1;
    puts("production present semaphore ownership and resource lifecycle: PASS");
    return 0;
}
