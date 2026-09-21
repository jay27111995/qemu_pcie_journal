/*
 * edu-pci Descriptor Ring DMA test via VFIO
 *
 * Tests the BAR4 ring-based DMA engine.
 *
 * Compile: g++ -o test_ring_dma test_ring_dma.cpp
 * Run: ./test_ring_dma
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <time.h>
#include <atomic>
#include <linux/vfio.h>

constexpr size_t PAGE_SIZE = 4096;

/*
 * Memory barriers for DMA ordering
 *
 * wmb() - Write memory barrier: ensures all writes before the barrier
 *         are visible before any writes after the barrier.
 *         Used before doorbell to ensure descriptors are in memory.
 *
 * rmb() - Read memory barrier: ensures all reads before the barrier
 *         complete before any reads after the barrier.
 *         Used after reading completion status.
 *
 * mb()  - Full memory barrier: both read and write ordering.
 */
#define wmb() std::atomic_thread_fence(std::memory_order_release)
#define rmb() std::atomic_thread_fence(std::memory_order_acquire)
#define mb()  std::atomic_thread_fence(std::memory_order_seq_cst)

/* BAR4: Ring DMA registers */
constexpr uint32_t REG_RING_ID       = 0x00;
constexpr uint32_t REG_RING_STATUS   = 0x04;
constexpr uint32_t REG_RING_ADDR_LO  = 0x08;
constexpr uint32_t REG_RING_ADDR_HI  = 0x0C;
constexpr uint32_t REG_RING_SIZE     = 0x10;
constexpr uint32_t REG_RING_HEAD     = 0x14;
constexpr uint32_t REG_RING_TAIL     = 0x18;
constexpr uint32_t REG_RING_CTRL     = 0x1C;

constexpr uint32_t RING_CTRL_ENABLE = (1 << 0);

/* Descriptor flags */
constexpr uint32_t DESC_FLAG_VALID = (1 << 0);
constexpr uint32_t DESC_FLAG_INT   = (1 << 1);

/* Descriptor status */
constexpr uint32_t DESC_STATUS_PENDING  = 0;
constexpr uint32_t DESC_STATUS_COMPLETE = 1;
constexpr uint32_t DESC_STATUS_ERROR    = 2;

/* DMA Descriptor - must match device (32 bytes) */
struct DmaDescriptor {
    uint32_t src_addr_lo;
    uint32_t src_addr_hi;
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
    uint32_t length;
    uint32_t flags;
    uint32_t status;
    uint32_t reserved;
};
static_assert(sizeof(DmaDescriptor) == 32, "Descriptor must be 32 bytes");

constexpr int NUM_DESCRIPTORS = 16;
constexpr uint64_t RING_IOVA   = 0x100000;  /* IOVA for descriptor ring */
constexpr uint64_t DATA_IOVA   = 0x200000;  /* IOVA for data buffers */

/* Timing helper */
static inline uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

class RingDmaDevice {
    int container_fd = -1;
    int group_fd = -1;
    int device_fd = -1;
    int event_fd = -1;
    int epoll_fd = -1;
    
    volatile uint32_t* bar4 = nullptr;
    size_t bar4_size = 0;
    
    /* Descriptor ring */
    DmaDescriptor* ring = nullptr;
    
    /* Data buffers (source and destination for each descriptor) */
    void* data_buf = nullptr;
    
public:
    ~RingDmaDevice() { cleanup(); }
    
    bool open(const char* pci_addr) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
        char link[256];
        ssize_t len = readlink(path, link, sizeof(link) - 1);
        if (len < 0) { perror("readlink"); return false; }
        link[len] = 0;
        int group_num = atoi(strrchr(link, '/') + 1);
        printf("Device %s in IOMMU group %d\n", pci_addr, group_num);
        
        container_fd = ::open("/dev/vfio/vfio", O_RDWR);
        snprintf(path, sizeof(path), "/dev/vfio/%d", group_num);
        group_fd = ::open(path, O_RDWR);
        if (container_fd < 0 || group_fd < 0) { perror("open vfio"); return false; }
        
        ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd);
        ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
        
        device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
        if (device_fd < 0) { perror("get device fd"); return false; }
        
        /* Map BAR4 (ring DMA engine) */
        vfio_region_info reg_info = { .argsz = sizeof(reg_info), .index = 4 };
        if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info) < 0) {
            perror("get bar4 info");
            return false;
        }
        bar4_size = reg_info.size;
        
        bar4 = (volatile uint32_t*)mmap(nullptr, bar4_size, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, device_fd, reg_info.offset);
        if (bar4 == MAP_FAILED) { perror("mmap bar4"); return false; }
        
        /* Enable bus master */
        vfio_region_info cfg = { .argsz = sizeof(cfg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg);
        uint16_t cmd;
        pread(device_fd, &cmd, 2, cfg.offset + 4);
        cmd |= 0x0007;
        pwrite(device_fd, &cmd, 2, cfg.offset + 4);
        
        printf("BAR4 mapped at %p, size=%zu\n", bar4, bar4_size);
        printf("Ring ID: 0x%08X\n", read_reg(REG_RING_ID));
        return true;
    }
    
    bool setup_msix() {
        event_fd = eventfd(0, 0);
        if (event_fd < 0) { perror("eventfd"); return false; }
        
        epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) { perror("epoll_create1"); return false; }
        
        epoll_event ev = { .events = EPOLLIN, .data = { .fd = event_fd } };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev) < 0) {
            perror("epoll_ctl"); return false;
        }
        
        /* Setup MSI-X vector 1 (ring DMA uses vector 1) */
        struct {
            vfio_irq_set set;
            int fd;
        } irq = {};
        irq.set.argsz = sizeof(irq);
        irq.set.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
        irq.set.index = VFIO_PCI_MSIX_IRQ_INDEX;
        irq.set.start = 1;  /* Vector 1 for ring DMA */
        irq.set.count = 1;
        irq.fd = event_fd;
        
        if (ioctl(device_fd, VFIO_DEVICE_SET_IRQS, &irq) < 0) {
            perror("set irqs"); return false;
        }
        printf("MSI-X vector 1 -> eventfd %d\n", event_fd);
        return true;
    }
    
    bool wait_interrupt(int timeout_ms = 1000) {
        epoll_event ev;
        int ret = epoll_wait(epoll_fd, &ev, 1, timeout_ms);
        if (ret > 0) {
            uint64_t val;
            read(event_fd, &val, sizeof(val));
            return true;
        }
        return false;
    }
    
    bool setup_ring() {
        /* Allocate descriptor ring */
        ring = (DmaDescriptor*)mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ring == MAP_FAILED) { perror("mmap ring"); return false; }
        memset(ring, 0, PAGE_SIZE);
        
        /* Allocate data buffers (src + dst for each descriptor) */
        data_buf = mmap(nullptr, PAGE_SIZE * NUM_DESCRIPTORS * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data_buf == MAP_FAILED) { perror("mmap data"); return false; }
        
        /* Map ring to IOMMU */
        vfio_iommu_type1_dma_map map_ring = {
            .argsz = sizeof(map_ring),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)ring,
            .iova = RING_IOVA,
            .size = PAGE_SIZE
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map_ring) < 0) {
            perror("map ring"); return false;
        }
        
        /* Map data buffers to IOMMU */
        vfio_iommu_type1_dma_map map_data = {
            .argsz = sizeof(map_data),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)data_buf,
            .iova = DATA_IOVA,
            .size = PAGE_SIZE * NUM_DESCRIPTORS * 2
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map_data) < 0) {
            perror("map data"); return false;
        }
        
        printf("Ring at vaddr=%p iova=0x%lx\n", ring, RING_IOVA);
        printf("Data at vaddr=%p iova=0x%lx\n", data_buf, DATA_IOVA);
        
        /* Configure the ring */
        write_reg(REG_RING_ADDR_LO, RING_IOVA & 0xFFFFFFFF);
        write_reg(REG_RING_ADDR_HI, RING_IOVA >> 32);
        write_reg(REG_RING_SIZE, NUM_DESCRIPTORS);
        write_reg(REG_RING_CTRL, RING_CTRL_ENABLE);
        
        printf("Ring configured: size=%d, enabled\n", NUM_DESCRIPTORS);
        return true;
    }
    
    uint32_t read_reg(uint32_t off) { return bar4[off / 4]; }
    void write_reg(uint32_t off, uint32_t val) { bar4[off / 4] = val; }
    
    void* get_src_buf(int idx) {
        return (uint8_t*)data_buf + (idx * 2) * PAGE_SIZE;
    }
    
    void* get_dst_buf(int idx) {
        return (uint8_t*)data_buf + (idx * 2 + 1) * PAGE_SIZE;
    }
    
    uint64_t get_src_iova(int idx) {
        return DATA_IOVA + (idx * 2) * PAGE_SIZE;
    }
    
    uint64_t get_dst_iova(int idx) {
        return DATA_IOVA + (idx * 2 + 1) * PAGE_SIZE;
    }
    
    bool test_single_descriptor() {
        printf("\n=== Test 1: Single Descriptor ===\n");
        
        /* Prepare source data */
        const char* msg = "Hello Ring DMA!";
        void* src = get_src_buf(0);
        void* dst = get_dst_buf(0);
        memcpy(src, msg, strlen(msg) + 1);
        memset(dst, 0, PAGE_SIZE);
        
        /* Build descriptor */
        uint64_t src_iova = get_src_iova(0);
        uint64_t dst_iova = get_dst_iova(0);
        
        ring[0].src_addr_lo = src_iova & 0xFFFFFFFF;
        ring[0].src_addr_hi = src_iova >> 32;
        ring[0].dst_addr_lo = dst_iova & 0xFFFFFFFF;
        ring[0].dst_addr_hi = dst_iova >> 32;
        ring[0].length = strlen(msg) + 1;
        ring[0].flags = DESC_FLAG_VALID | DESC_FLAG_INT;
        ring[0].status = DESC_STATUS_PENDING;
        
        printf("Descriptor 0: src=0x%lx dst=0x%lx len=%zu flags=0x%x\n",
               src_iova, dst_iova, strlen(msg) + 1, ring[0].flags);
        
        /* Write barrier: ensure descriptor is in memory before doorbell */
        wmb();
        
        /* Ring doorbell and measure time */
        printf("Ringing doorbell: tail=1\n");
        uint64_t t_start = get_time_ns();
        write_reg(REG_RING_TAIL, 1);
        
        /* Wait for interrupt */
        if (wait_interrupt()) {
            uint64_t t_end = get_time_ns();
            uint64_t elapsed_us = (t_end - t_start) / 1000;
            printf("Got interrupt! Latency: %lu us\n", elapsed_us);
        } else {
            printf("Timeout waiting for interrupt\n");
        }
        
        /* Read barrier: ensure we see updated status/data from device */
        rmb();
        
        /* Check status */
        printf("Descriptor status: %u (1=complete)\n", ring[0].status);
        printf("Head: %u, Tail: %u\n", read_reg(REG_RING_HEAD), read_reg(REG_RING_TAIL));
        
        /* Verify data */
        if (memcmp(src, dst, strlen(msg) + 1) == 0) {
            printf("SUCCESS: '%s'\n", (char*)dst);
            return true;
        } else {
            printf("FAILED: data mismatch\n");
            printf("  src: %s\n", (char*)src);
            printf("  dst: %s\n", (char*)dst);
            return false;
        }
    }
    
    bool test_batch_descriptors() {
        printf("\n=== Test 2: Batch of 4 Descriptors ===\n");
        
        const char* messages[] = {
            "Message 0 - First",
            "Message 1 - Second",
            "Message 2 - Third",
            "Message 3 - Fourth (with interrupt)"
        };
        
        /* Get current head position - we continue from here */
        uint32_t start_idx = read_reg(REG_RING_HEAD);
        printf("Starting at ring index %u\n", start_idx);
        
        /* Build 4 descriptors starting from current head */
        for (int i = 0; i < 4; i++) {
            uint32_t ring_idx = (start_idx + i) % NUM_DESCRIPTORS;
            void* src = get_src_buf(ring_idx);
            void* dst = get_dst_buf(ring_idx);
            memcpy(src, messages[i], strlen(messages[i]) + 1);
            memset(dst, 0, PAGE_SIZE);
            
            uint64_t src_iova = get_src_iova(ring_idx);
            uint64_t dst_iova = get_dst_iova(ring_idx);
            
            ring[ring_idx].src_addr_lo = src_iova & 0xFFFFFFFF;
            ring[ring_idx].src_addr_hi = src_iova >> 32;
            ring[ring_idx].dst_addr_lo = dst_iova & 0xFFFFFFFF;
            ring[ring_idx].dst_addr_hi = dst_iova >> 32;
            ring[ring_idx].length = strlen(messages[i]) + 1;
            ring[ring_idx].flags = DESC_FLAG_VALID;
            if (i == 3) {
                ring[ring_idx].flags |= DESC_FLAG_INT;  /* Interrupt on last one */
            }
            ring[ring_idx].status = DESC_STATUS_PENDING;
            
            printf("Desc[%u]: len=%zu flags=0x%x\n", ring_idx, strlen(messages[i]) + 1, ring[ring_idx].flags);
        }
        
        /* Write barrier: ensure all descriptors are in memory before doorbell */
        wmb();
        
        /* Ring doorbell: tail = start + 4 */
        uint32_t new_tail = (start_idx + 4) % NUM_DESCRIPTORS;
        printf("Ringing doorbell: tail=%u\n", new_tail);
        uint64_t t_start = get_time_ns();
        write_reg(REG_RING_TAIL, new_tail);
        
        /* Wait for interrupt (only on last descriptor) */
        if (wait_interrupt()) {
            uint64_t t_end = get_time_ns();
            uint64_t elapsed_us = (t_end - t_start) / 1000;
            printf("Got interrupt! Latency: %lu us (4 descriptors)\n", elapsed_us);
        } else {
            printf("Timeout waiting for interrupt\n");
        }
        
        /* Read barrier: ensure we see updated status/data from device */
        rmb();
        
        /* Check all descriptors */
        printf("Head: %u\n", read_reg(REG_RING_HEAD));
        
        bool all_ok = true;
        for (int i = 0; i < 4; i++) {
            uint32_t ring_idx = (start_idx + i) % NUM_DESCRIPTORS;
            void* src = get_src_buf(ring_idx);
            void* dst = get_dst_buf(ring_idx);
            bool ok = memcmp(src, dst, strlen(messages[i]) + 1) == 0;
            printf("Desc[%u]: status=%u %s -> '%s'\n",
                   ring_idx, ring[ring_idx].status, ok ? "OK" : "FAIL", (char*)dst);
            all_ok = all_ok && ok;
        }
        
        return all_ok;
    }
    
    void cleanup() {
        if (bar4 && bar4 != MAP_FAILED) munmap((void*)bar4, bar4_size);
        if (ring && ring != MAP_FAILED) munmap(ring, PAGE_SIZE);
        if (data_buf && data_buf != MAP_FAILED) munmap(data_buf, PAGE_SIZE * NUM_DESCRIPTORS * 2);
        if (epoll_fd >= 0) close(epoll_fd);
        if (event_fd >= 0) close(event_fd);
        if (device_fd >= 0) close(device_fd);
        if (group_fd >= 0) close(group_fd);
        if (container_fd >= 0) close(container_fd);
    }
};

int main() {
    const char* pci_addr = "0000:00:04.0";
    
    RingDmaDevice dev;
    if (!dev.open(pci_addr)) return 1;
    if (!dev.setup_msix()) return 1;
    if (!dev.setup_ring()) return 1;
    
    bool ok = true;
    ok = dev.test_single_descriptor() && ok;
    ok = dev.test_batch_descriptors() && ok;
    
    printf("\n=== %s ===\n", ok ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return ok ? 0 : 1;
}
