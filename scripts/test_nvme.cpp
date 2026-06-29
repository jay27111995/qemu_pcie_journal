/*
 * NVMe test via VFIO in C++
 * Demonstrates submission/completion queue flow
 *
 * Compile: g++ -o test_nvme test_nvme.cpp
 * Run: ./test_nvme
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
#include <sys/select.h>
#include <linux/vfio.h>

// NVMe registers
constexpr uint32_t NVME_CAP      = 0x00;
constexpr uint32_t NVME_VS       = 0x08;
constexpr uint32_t NVME_CC       = 0x14;
constexpr uint32_t NVME_CSTS     = 0x1C;
constexpr uint32_t NVME_AQA      = 0x24;
constexpr uint32_t NVME_ASQ      = 0x28;
constexpr uint32_t NVME_ACQ      = 0x30;
constexpr uint32_t NVME_SQ0TDBL  = 0x1000;
constexpr uint32_t NVME_CQ0HDBL  = 0x1004;

// NVMe opcodes
constexpr uint8_t NVME_CMD_READ     = 0x02;
constexpr uint8_t NVME_CMD_WRITE    = 0x01;
constexpr uint8_t NVME_CMD_IDENTIFY = 0x06;

constexpr int QUEUE_SIZE = 16;
constexpr size_t PAGE_SIZE = 4096;

struct NvmeCmd {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct NvmeCqe {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

class VfioDevice {
    int container_fd = -1;
    int group_fd = -1;
    int device_fd = -1;
    int event_fd = -1;
    void* bar0 = nullptr;
    size_t bar0_size = 0;
    void* dma_mem = nullptr;
    uint64_t dma_iova = 0x100000;
    size_t dma_size = PAGE_SIZE * 4;

public:
    ~VfioDevice() { cleanup(); }

    bool open(const char* pci_addr) {
        // Get IOMMU group
        char path[256];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
        char link[256];
        ssize_t len = readlink(path, link, sizeof(link) - 1);
        if (len < 0) { perror("readlink"); return false; }
        link[len] = 0;
        int group_num = atoi(strrchr(link, '/') + 1);

        // Open container
        container_fd = ::open("/dev/vfio/vfio", O_RDWR);
        if (container_fd < 0) { perror("open container"); return false; }

        // Open group
        snprintf(path, sizeof(path), "/dev/vfio/%d", group_num);
        group_fd = ::open(path, O_RDWR);
        if (group_fd < 0) { perror("open group"); return false; }

        // Set container
        ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd);
        ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

        // Get device fd
        device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
        if (device_fd < 0) { perror("get device fd"); return false; }

        // Get BAR0 info
        vfio_region_info reg_info = { .argsz = sizeof(reg_info), .index = 0 };
        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
        bar0_size = reg_info.size;

        // mmap BAR0
        bar0 = mmap(nullptr, bar0_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    device_fd, reg_info.offset);
        if (bar0 == MAP_FAILED) { perror("mmap bar0"); return false; }

        // Enable bus master via config space
        vfio_region_info cfg_info = { .argsz = sizeof(cfg_info), .index = VFIO_PCI_CONFIG_REGION_INDEX };
        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg_info);
        uint16_t cmd;
        pread(device_fd, &cmd, 2, cfg_info.offset + 4);
        cmd |= 0x0007;  // IO + Memory + Bus Master
        pwrite(device_fd, &cmd, 2, cfg_info.offset + 4);

        printf("BAR0 mapped: size=%zu, bus master enabled\n", bar0_size);
        return true;
    }

    bool setup_msix() {
        event_fd = eventfd(0, 0);
        if (event_fd < 0) return false;

        // Get IRQ info
        vfio_irq_info irq_info = { .argsz = sizeof(irq_info), .index = VFIO_PCI_MSIX_IRQ_INDEX };
        ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
        printf("MSI-X vectors: %d\n", irq_info.count);

        // Enable MSI-X with eventfd
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
            perror("set irqs");
            return false;
        }
        printf("MSI-X enabled\n");
        return true;
    }

    bool setup_dma() {
        dma_mem = mmap(nullptr, dma_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (dma_mem == MAP_FAILED) return false;

        vfio_iommu_type1_dma_map dma_map = {
            .argsz = sizeof(dma_map),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)dma_mem,
            .iova = dma_iova,
            .size = dma_size
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
            perror("map dma");
            return false;
        }
        printf("DMA mapped: vaddr=%p iova=0x%lx size=%zu\n",
               dma_mem, dma_iova, dma_size);
        return true;
    }

    // Register access
    uint32_t read32(uint32_t off) { return *(volatile uint32_t*)((char*)bar0 + off); }
    void write32(uint32_t off, uint32_t val) { *(volatile uint32_t*)((char*)bar0 + off) = val; }
    uint64_t read64(uint32_t off) { return *(volatile uint64_t*)((char*)bar0 + off); }
    void write64(uint32_t off, uint64_t val) { *(volatile uint64_t*)((char*)bar0 + off) = val; }

    // DMA memory access
    void* sq_ptr() { return dma_mem; }
    void* cq_ptr() { return (char*)dma_mem + PAGE_SIZE; }
    void* data_ptr() { return (char*)dma_mem + PAGE_SIZE * 2; }
    uint64_t sq_iova() { return dma_iova; }
    uint64_t cq_iova() { return dma_iova + PAGE_SIZE; }
    uint64_t data_iova() { return dma_iova + PAGE_SIZE * 2; }

    bool wait_interrupt(int timeout_ms = 2000) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(event_fd, &fds);
        timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(event_fd + 1, &fds, nullptr, nullptr, &tv) > 0) {
            uint64_t val;
            read(event_fd, &val, sizeof(val));
            return true;
        }
        return false;
    }

    void cleanup() {
        if (bar0 && bar0 != MAP_FAILED) munmap(bar0, bar0_size);
        if (dma_mem && dma_mem != MAP_FAILED) munmap(dma_mem, dma_size);
        if (event_fd >= 0) close(event_fd);
        if (device_fd >= 0) close(device_fd);
        if (group_fd >= 0) close(group_fd);
        if (container_fd >= 0) close(container_fd);
    }
};

class NvmeController {
    VfioDevice& dev;
    uint16_t sq_tail = 0;
    uint16_t cq_head = 0;
    uint16_t cmd_id = 1;

public:
    NvmeController(VfioDevice& d) : dev(d) {}

    bool init() {
        printf("\nCAP: 0x%016lx\n", dev.read64(NVME_CAP));
        printf("Version: %d.%d\n", (dev.read32(NVME_VS) >> 16) & 0xFF,
                                   (dev.read32(NVME_VS) >> 8) & 0xFF);

        // Configure queues
        dev.write32(NVME_AQA, ((QUEUE_SIZE - 1) << 16) | (QUEUE_SIZE - 1));
        dev.write64(NVME_ASQ, dev.sq_iova());
        dev.write64(NVME_ACQ, dev.cq_iova());

        // Enable controller
        dev.write32(NVME_CC, 0x00460001);
        printf("Enabling controller...\n");

        // Wait ready
        for (int i = 0; i < 100; i++) {
            if (dev.read32(NVME_CSTS) & 1) {
                printf("Controller ready!\n");
                return true;
            }
            usleep(1000);
        }
        printf("Controller not ready!\n");
        return false;
    }

    bool submit_cmd(NvmeCmd& cmd) {
        cmd.cid = cmd_id++;
        memcpy((char*)dev.sq_ptr() + sq_tail * sizeof(NvmeCmd), &cmd, sizeof(cmd));
        sq_tail = (sq_tail + 1) % QUEUE_SIZE;
        dev.write32(NVME_SQ0TDBL, sq_tail);
        return true;
    }

    bool wait_completion(NvmeCqe& cqe) {
        if (!dev.wait_interrupt()) {
            printf("Timeout!\n");
            return false;
        }
        memcpy(&cqe, (char*)dev.cq_ptr() + cq_head * sizeof(NvmeCqe), sizeof(cqe));
        cq_head = (cq_head + 1) % QUEUE_SIZE;
        dev.write32(NVME_CQ0HDBL, cq_head);
        return true;
    }

    bool identify() {
        printf("\n=== Identify Controller ===\n");
        NvmeCmd cmd = {};
        cmd.opcode = NVME_CMD_IDENTIFY;
        cmd.prp1 = dev.data_iova();
        cmd.cdw10 = 1;  // CNS=1: identify controller

        submit_cmd(cmd);
        NvmeCqe cqe;
        if (!wait_completion(cqe)) return false;

        printf("Status: 0x%04x\n", cqe.status & 0xFF);
        uint8_t* id = (uint8_t*)dev.data_ptr();
        printf("Vendor: 0x%04x\n", *(uint16_t*)id);
        printf("Serial: %.20s\n", id + 4);
        printf("Model: %.40s\n", id + 24);
        return true;
    }

    bool write_data(uint64_t lba, const void* data, size_t len) {
        printf("\n=== Write LBA %lu ===\n", lba);
        memcpy(dev.data_ptr(), data, len);

        NvmeCmd cmd = {};
        cmd.opcode = NVME_CMD_WRITE;
        cmd.nsid = 1;
        cmd.prp1 = dev.data_iova();
        cmd.cdw10 = lba;
        cmd.cdw12 = 0;  // 1 block

        submit_cmd(cmd);
        NvmeCqe cqe;
        if (!wait_completion(cqe)) return false;
        printf("Write status: 0x%04x\n", cqe.status & 0xFF);
        return (cqe.status & 0xFF) == 0;
    }

    bool read_data(uint64_t lba, void* data, size_t len) {
        printf("\n=== Read LBA %lu ===\n", lba);
        memset(dev.data_ptr(), 0, len);

        NvmeCmd cmd = {};
        cmd.opcode = NVME_CMD_READ;
        cmd.nsid = 1;
        cmd.prp1 = dev.data_iova();
        cmd.cdw10 = lba;
        cmd.cdw12 = 0;  // 1 block

        submit_cmd(cmd);
        NvmeCqe cqe;
        if (!wait_completion(cqe)) return false;
        printf("Read status: 0x%04x\n", cqe.status & 0xFF);
        memcpy(data, dev.data_ptr(), len);
        return (cqe.status & 0xFF) == 0;
    }
};

int main() {
    const char* pci_addr = "0000:00:03.0";

    VfioDevice dev;
    if (!dev.open(pci_addr)) return 1;
    if (!dev.setup_msix()) return 1;
    if (!dev.setup_dma()) return 1;

    NvmeController nvme(dev);
    if (!nvme.init()) return 1;

    nvme.identify();

    // Write test
    const char* test_str = "Hello NVMe from C++!";
    nvme.write_data(0, test_str, strlen(test_str) + 1);

    // Read back
    char buf[512] = {};
    nvme.read_data(0, buf, sizeof(buf));
    printf("\nRead back: %s\n", buf);

    if (strcmp(buf, test_str) == 0) {
        printf("\n*** SUCCESS: Data matches! ***\n");
    } else {
        printf("\n*** FAIL: Data mismatch! ***\n");
    }

    return 0;
}
