/*
 * edu-pci DMA + MSI-X test via VFIO in C++
 *
 * Compile: g++ -o test_edu_dma test_edu_dma.cpp
 * Run: ./test_edu_dma
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
#include <linux/vfio.h>

// edu-pci registers (BAR0)
constexpr uint32_t REG_ID        = 0x00;
constexpr uint32_t REG_SCRATCH   = 0x04;
constexpr uint32_t REG_STATUS    = 0x0C;
constexpr uint32_t REG_DMA_SRC   = 0x10;
constexpr uint32_t REG_DMA_DST   = 0x14;
constexpr uint32_t REG_DMA_LEN   = 0x18;
constexpr uint32_t REG_DMA_CMD   = 0x1C;
constexpr uint32_t REG_IRQ_RAISE = 0x24;

constexpr size_t PAGE_SIZE = 4096;

class EduPciDevice {
    int container_fd = -1;
    int group_fd = -1;
    int device_fd = -1;
    int event_fd = -1;
    int epoll_fd = -1;
    volatile uint32_t* bar0 = nullptr;
    size_t bar0_size = 0;

    void* src_buf = nullptr;
    void* dst_buf = nullptr;
    uint64_t src_iova = 0x10000;
    uint64_t dst_iova = 0x20000;

public:
    ~EduPciDevice() { cleanup(); }

    bool open(const char* pci_addr) {
        // Get IOMMU group
        char path[256];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
        char link[256];
        ssize_t len = readlink(path, link, sizeof(link) - 1);
        if (len < 0) { perror("readlink"); return false; }
        link[len] = 0;
        int group_num = atoi(strrchr(link, '/') + 1);
        printf("Device %s in IOMMU group %d\n", pci_addr, group_num);

        // Open container and group
        container_fd = ::open("/dev/vfio/vfio", O_RDWR);
        snprintf(path, sizeof(path), "/dev/vfio/%d", group_num);
        group_fd = ::open(path, O_RDWR);
        if (container_fd < 0 || group_fd < 0) { perror("open vfio"); return false; }

        ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd);
        ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

        // Get device fd
        device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
        if (device_fd < 0) { perror("get device fd"); return false; }

        // Map BAR0
        vfio_region_info reg_info = { .argsz = sizeof(reg_info), .index = 0 };
        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
        bar0_size = reg_info.size;

        bar0 = (volatile uint32_t*)mmap(nullptr, bar0_size, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, device_fd, reg_info.offset);
        if (bar0 == MAP_FAILED) { perror("mmap bar0"); return false; }

        // Enable bus master
        vfio_region_info cfg = { .argsz = sizeof(cfg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg);
        uint16_t cmd;
        pread(device_fd, &cmd, 2, cfg.offset + 4);
        cmd |= 0x0007;
        pwrite(device_fd, &cmd, 2, cfg.offset + 4);

        printf("BAR0 mapped at %p, size=%zu\n", bar0, bar0_size);
        printf("Device ID: 0x%08X\n", read_reg(REG_ID));
        return true;
    }

    bool setup_msix() {
        // Create eventfd for interrupt notification
        event_fd = eventfd(0, 0);
        if (event_fd < 0) { perror("eventfd"); return false; }

        // Create epoll instance and add eventfd
        epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) { perror("epoll_create1"); return false; }

        epoll_event ev = { .events = EPOLLIN, .data = { .fd = event_fd } };
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev) < 0) {
            perror("epoll_ctl"); return false;
        }

        // Get MSI-X info
        vfio_irq_info irq_info = { .argsz = sizeof(irq_info), .index = VFIO_PCI_MSIX_IRQ_INDEX };
        if (ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info) < 0) {
            perror("get irq info"); return false;
        }
        printf("MSI-X vectors available: %d\n", irq_info.count);

        // Setup MSI-X vector 0 with eventfd
        struct {
            vfio_irq_set set;
            int fd;
        } irq = {};
        irq.set.argsz = sizeof(irq);
        irq.set.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
        irq.set.index = VFIO_PCI_MSIX_IRQ_INDEX;
        irq.set.start = 0;
        irq.set.count = 1;
        irq.fd = event_fd;

        if (ioctl(device_fd, VFIO_DEVICE_SET_IRQS, &irq) < 0) {
            perror("set irqs"); return false;
        }
        printf("MSI-X vector 0 -> eventfd %d (epoll_fd %d)\n", event_fd, epoll_fd);
        return true;
    }

    bool wait_interrupt(int timeout_ms = 1000) {
        epoll_event ev;
        int ret = epoll_wait(epoll_fd, &ev, 1, timeout_ms);
        if (ret > 0) {
            uint64_t val;
            read(event_fd, &val, sizeof(val));  // Clear eventfd
            return true;
        }
        return false;
    }

    bool setup_dma() {
        // Allocate src and dst buffers
        src_buf = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        dst_buf = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (src_buf == MAP_FAILED || dst_buf == MAP_FAILED) return false;

        // Map src buffer
        vfio_iommu_type1_dma_map map_src = {
            .argsz = sizeof(map_src),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)src_buf,
            .iova = src_iova,
            .size = PAGE_SIZE
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map_src) < 0) {
            perror("map src"); return false;
        }

        // Map dst buffer
        vfio_iommu_type1_dma_map map_dst = {
            .argsz = sizeof(map_dst),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)dst_buf,
            .iova = dst_iova,
            .size = PAGE_SIZE
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map_dst) < 0) {
            perror("map dst"); return false;
        }

        printf("DMA buffers mapped: src_iova=0x%lx dst_iova=0x%lx\n", src_iova, dst_iova);
        return true;
    }

    uint32_t read_reg(uint32_t off) { return bar0[off / 4]; }
    void write_reg(uint32_t off, uint32_t val) { bar0[off / 4] = val; }

    bool do_dma(const void* data, size_t len) {
        if (len > PAGE_SIZE) len = PAGE_SIZE;

        // Write test data to src buffer
        memcpy(src_buf, data, len);
        memset(dst_buf, 0, PAGE_SIZE);

        // Program DMA registers
        write_reg(REG_DMA_SRC, src_iova);
        write_reg(REG_DMA_DST, dst_iova);
        write_reg(REG_DMA_LEN, len);

        printf("Starting DMA: src=0x%lx dst=0x%lx len=%zu\n", src_iova, dst_iova, len);

        // Start DMA (device sends MSI-X on completion)
        write_reg(REG_DMA_CMD, 1);

        // Wait for MSI-X interrupt
        if (wait_interrupt()) {
            printf("Got MSI-X interrupt!\n");
        } else {
            printf("Timeout waiting for interrupt\n");
        }

        // Check result
        if (memcmp(src_buf, dst_buf, len) == 0) {
            printf("DMA SUCCESS: data copied correctly\n");
            return true;
        } else {
            printf("DMA FAILED: data mismatch\n");
            printf("  src: %.32s\n", (char*)src_buf);
            printf("  dst: %.32s\n", (char*)dst_buf);
            return false;
        }
    }

    void test_manual_interrupt(int vector) {
        printf("Raising MSI-X vector %d manually...\n", vector);
        write_reg(REG_IRQ_RAISE, vector);
        if (wait_interrupt()) {
            printf("Got MSI-X interrupt!\n");
        } else {
            printf("No interrupt received\n");
        }
    }

    void cleanup() {
        if (bar0 && bar0 != MAP_FAILED) munmap((void*)bar0, bar0_size);
        if (src_buf && src_buf != MAP_FAILED) munmap(src_buf, PAGE_SIZE);
        if (dst_buf && dst_buf != MAP_FAILED) munmap(dst_buf, PAGE_SIZE);
        if (epoll_fd >= 0) close(epoll_fd);
        if (event_fd >= 0) close(event_fd);
        if (device_fd >= 0) close(device_fd);
        if (group_fd >= 0) close(group_fd);
        if (container_fd >= 0) close(container_fd);
    }
};

int main() {
    const char* pci_addr = "0000:00:04.0";

    EduPciDevice dev;
    if (!dev.open(pci_addr)) return 1;
    if (!dev.setup_msix()) return 1;
    if (!dev.setup_dma()) return 1;

    // Test scratch register
    dev.write_reg(REG_SCRATCH, 0xDEADBEEF);
    printf("Scratch: wrote 0xDEADBEEF, read 0x%08X\n\n", dev.read_reg(REG_SCRATCH));

    // Test manual MSI-X
    dev.test_manual_interrupt(0);
    printf("\n");

    // Test DMA with interrupt
    const char* test_data = "Hello edu-pci DMA from C++!";
    dev.do_dma(test_data, strlen(test_data) + 1);

    return 0;
}
