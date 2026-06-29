/*
 * ATS test for edu-pci device
 *
 * Compile: g++ -o test_ats test_ats.cpp
 * Run: ./test_ats
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>

// edu-pci registers
constexpr uint32_t REG_ID       = 0x00;
constexpr uint32_t REG_DMA_SRC  = 0x10;
constexpr uint32_t REG_ATS_ADDR = 0x38;
constexpr uint32_t REG_ATS_CMD  = 0x3C;

constexpr size_t PAGE_SIZE = 4096;

int main() {
    const char* pci_addr = "0000:00:04.0";

    // Get IOMMU group
    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
    char link[256];
    ssize_t len = readlink(path, link, sizeof(link) - 1);
    if (len < 0) { perror("readlink"); return 1; }
    link[len] = 0;
    int group_num = atoi(strrchr(link, '/') + 1);
    printf("Device %s in IOMMU group %d\n", pci_addr, group_num);

    // Open VFIO
    int container = open("/dev/vfio/vfio", O_RDWR);
    snprintf(path, sizeof(path), "/dev/vfio/%d", group_num);
    int group = open(path, O_RDWR);
    
    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);
    ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
    
    int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
    if (device < 0) { perror("get device fd"); return 1; }

    // Map BAR0
    vfio_region_info reg_info = { .argsz = sizeof(reg_info), .index = 0 };
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
    
    volatile uint32_t* bar0 = (volatile uint32_t*)mmap(nullptr, reg_info.size,
        PROT_READ | PROT_WRITE, MAP_SHARED, device, reg_info.offset);
    if (bar0 == MAP_FAILED) { perror("mmap"); return 1; }

    printf("Device ID: 0x%08X\n", bar0[REG_ID / 4]);

    // Allocate a buffer and map it for DMA
    void* buf = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(buf, 0x42, PAGE_SIZE);  // Touch it

    uint64_t iova = 0x100000;
    vfio_iommu_type1_dma_map dma_map = {
        .argsz = sizeof(dma_map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uint64_t)buf,
        .iova = iova,
        .size = PAGE_SIZE
    };
    if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
        perror("map dma"); return 1;
    }
    printf("DMA mapped: iova=0x%lx\n\n", iova);

    // Test ATS: ask device to translate the IOVA we just mapped
    printf("=== ATS Test ===\n");
    printf("Requesting translation for IOVA 0x%lx...\n", iova);
    
    bar0[REG_ATS_ADDR / 4] = iova;    // Set address to translate
    bar0[REG_ATS_CMD / 4] = 1;        // Trigger ATS request
    
    printf("Check QEMU terminal for [TLP] ATS TransReq/TransCmpl!\n\n");

    // Test ATS with unmapped address (should fail)
    printf("=== ATS Test (unmapped address) ===\n");
    uint64_t bad_iova = 0x999000;
    printf("Requesting translation for unmapped IOVA 0x%lx...\n", bad_iova);
    
    bar0[REG_ATS_ADDR / 4] = bad_iova;
    bar0[REG_ATS_CMD / 4] = 1;
    
    printf("Check QEMU terminal - should show FAILED!\n");

    // Cleanup
    munmap((void*)bar0, reg_info.size);
    munmap(buf, PAGE_SIZE);
    close(device);
    close(group);
    close(container);
    
    return 0;
}
