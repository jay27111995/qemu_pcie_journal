/*
 * edu-pci AES Crypto Engine test via VFIO
 *
 * Tests the BAR5 AES-128-CBC crypto engine.
 *
 * Compile: g++ -o test_crypto test_crypto.cpp
 * Run: ./test_crypto
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

/* Memory barriers */
#define wmb() std::atomic_thread_fence(std::memory_order_release)
#define rmb() std::atomic_thread_fence(std::memory_order_acquire)

/* BAR5: Crypto registers */
constexpr uint32_t REG_CRYPTO_ID      = 0x00;
constexpr uint32_t REG_CRYPTO_STATUS  = 0x04;
constexpr uint32_t REG_CRYPTO_SRC_LO  = 0x08;
constexpr uint32_t REG_CRYPTO_SRC_HI  = 0x0C;
constexpr uint32_t REG_CRYPTO_DST_LO  = 0x10;
constexpr uint32_t REG_CRYPTO_DST_HI  = 0x14;
constexpr uint32_t REG_CRYPTO_LEN     = 0x18;
constexpr uint32_t REG_CRYPTO_CMD     = 0x1C;
constexpr uint32_t REG_CRYPTO_KEY     = 0x20;  /* 16 bytes */
constexpr uint32_t REG_CRYPTO_IV      = 0x30;  /* 16 bytes */

constexpr uint32_t CRYPTO_CMD_START   = (1 << 0);
constexpr uint32_t CRYPTO_CMD_ENCRYPT = (1 << 1);

constexpr uint64_t SRC_IOVA = 0x300000;
constexpr uint64_t DST_IOVA = 0x301000;

/* Timing helper */
static inline uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Hex dump helper */
static void hexdump(const char* label, const uint8_t* data, size_t len) {
    printf("%s: ", label);
    for (size_t i = 0; i < len && i < 32; i++) {
        printf("%02x", data[i]);
    }
    if (len > 32) printf("...");
    printf("\n");
}

class CryptoDevice {
    int container_fd = -1;
    int group_fd = -1;
    int device_fd = -1;
    int event_fd = -1;
    int epoll_fd = -1;
    
    volatile uint32_t* bar5 = nullptr;
    size_t bar5_size = 0;
    
    void* src_buf = nullptr;
    void* dst_buf = nullptr;
    
public:
    ~CryptoDevice() { cleanup(); }
    
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
        
        /* Map BAR5 (crypto engine) */
        vfio_region_info reg_info = { .argsz = sizeof(reg_info), .index = 5 };
        if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info) < 0) {
            perror("get bar5 info");
            return false;
        }
        bar5_size = reg_info.size;
        
        bar5 = (volatile uint32_t*)mmap(nullptr, bar5_size, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, device_fd, reg_info.offset);
        if (bar5 == MAP_FAILED) { perror("mmap bar5"); return false; }
        
        /* Enable bus master */
        vfio_region_info cfg = { .argsz = sizeof(cfg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
        ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg);
        uint16_t cmd;
        pread(device_fd, &cmd, 2, cfg.offset + 4);
        cmd |= 0x0007;
        pwrite(device_fd, &cmd, 2, cfg.offset + 4);
        
        printf("BAR5 mapped at %p, size=%zu\n", bar5, bar5_size);
        printf("Crypto ID: 0x%08X\n", read_reg(REG_CRYPTO_ID));
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
        
        /* Setup MSI-X vectors 0, 1, 2 (crypto uses vector 2) */
        struct {
            vfio_irq_set set;
            int fds[3];
        } irq = {};
        irq.set.argsz = sizeof(irq);
        irq.set.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
        irq.set.index = VFIO_PCI_MSIX_IRQ_INDEX;
        irq.set.start = 0;
        irq.set.count = 3;  /* Must setup all vectors up to and including the one we want */
        irq.fds[0] = -1;    /* Don't care about vector 0 */
        irq.fds[1] = -1;    /* Don't care about vector 1 */
        irq.fds[2] = event_fd;  /* Vector 2 for crypto */
        
        if (ioctl(device_fd, VFIO_DEVICE_SET_IRQS, &irq) < 0) {
            perror("set irqs"); return false;
        }
        printf("MSI-X vector 2 -> eventfd %d\n", event_fd);
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
    
    bool setup_dma() {
        src_buf = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        dst_buf = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (src_buf == MAP_FAILED || dst_buf == MAP_FAILED) return false;
        
        vfio_iommu_type1_dma_map map_src = {
            .argsz = sizeof(map_src),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)src_buf,
            .iova = SRC_IOVA,
            .size = PAGE_SIZE
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map_src) < 0) {
            perror("map src"); return false;
        }
        
        vfio_iommu_type1_dma_map map_dst = {
            .argsz = sizeof(map_dst),
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            .vaddr = (uint64_t)dst_buf,
            .iova = DST_IOVA,
            .size = PAGE_SIZE
        };
        if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map_dst) < 0) {
            perror("map dst"); return false;
        }
        
        printf("DMA: src_iova=0x%lx dst_iova=0x%lx\n", SRC_IOVA, DST_IOVA);
        return true;
    }
    
    uint32_t read_reg(uint32_t off) { return bar5[off / 4]; }
    void write_reg(uint32_t off, uint32_t val) { bar5[off / 4] = val; }
    
    void set_key(const uint8_t* key) {
        /* Write 16-byte key as 4 x 32-bit writes */
        for (int i = 0; i < 16; i += 4) {
            uint32_t word;
            memcpy(&word, &key[i], 4);
            write_reg(REG_CRYPTO_KEY + i, word);
        }
        printf("Key set: ");
        for (int i = 0; i < 16; i++) printf("%02x", key[i]);
        printf("\n");
    }
    
    void set_iv(const uint8_t* iv) {
        for (int i = 0; i < 16; i += 4) {
            uint32_t word;
            memcpy(&word, &iv[i], 4);
            write_reg(REG_CRYPTO_IV + i, word);
        }
        printf("IV set:  ");
        for (int i = 0; i < 16; i++) printf("%02x", iv[i]);
        printf("\n");
    }
    
    bool encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len) {
        memcpy(src_buf, plaintext, len);
        memset(dst_buf, 0, PAGE_SIZE);
        
        write_reg(REG_CRYPTO_SRC_LO, SRC_IOVA & 0xFFFFFFFF);
        write_reg(REG_CRYPTO_SRC_HI, SRC_IOVA >> 32);
        write_reg(REG_CRYPTO_DST_LO, DST_IOVA & 0xFFFFFFFF);
        write_reg(REG_CRYPTO_DST_HI, DST_IOVA >> 32);
        write_reg(REG_CRYPTO_LEN, len);
        
        wmb();  /* Ensure all writes complete before starting */
        
        uint64_t t_start = get_time_ns();
        write_reg(REG_CRYPTO_CMD, CRYPTO_CMD_START | CRYPTO_CMD_ENCRYPT);
        
        if (wait_interrupt()) {
            uint64_t t_end = get_time_ns();
            printf("Encrypt complete! Latency: %lu us\n", (t_end - t_start) / 1000);
        } else {
            printf("Timeout waiting for encrypt\n");
            return false;
        }
        
        rmb();  /* Ensure we see the result */
        memcpy(ciphertext, dst_buf, len);
        return true;
    }
    
    bool decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len) {
        memcpy(src_buf, ciphertext, len);
        memset(dst_buf, 0, PAGE_SIZE);
        
        write_reg(REG_CRYPTO_SRC_LO, SRC_IOVA & 0xFFFFFFFF);
        write_reg(REG_CRYPTO_SRC_HI, SRC_IOVA >> 32);
        write_reg(REG_CRYPTO_DST_LO, DST_IOVA & 0xFFFFFFFF);
        write_reg(REG_CRYPTO_DST_HI, DST_IOVA >> 32);
        write_reg(REG_CRYPTO_LEN, len);
        
        wmb();
        
        uint64_t t_start = get_time_ns();
        write_reg(REG_CRYPTO_CMD, CRYPTO_CMD_START);  /* No ENCRYPT flag = decrypt */
        
        if (wait_interrupt()) {
            uint64_t t_end = get_time_ns();
            printf("Decrypt complete! Latency: %lu us\n", (t_end - t_start) / 1000);
        } else {
            printf("Timeout waiting for decrypt\n");
            return false;
        }
        
        rmb();
        memcpy(plaintext, dst_buf, len);
        return true;
    }
    
    bool test_encrypt_decrypt() {
        printf("\n=== AES-128-CBC Encrypt/Decrypt Test ===\n");
        
        /* Test key and IV */
        uint8_t key[16] = {
            0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
            0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
        };
        uint8_t iv[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };
        
        /* Plaintext (32 bytes = 2 AES blocks) */
        uint8_t plaintext[32] = "Hello AES Crypto Engine!1234567";  /* 31 chars + null */
        uint8_t ciphertext[32];
        uint8_t decrypted[32];
        
        set_key(key);
        set_iv(iv);
        
        hexdump("Plaintext ", plaintext, 32);
        
        /* Encrypt */
        printf("\n--- Encrypting ---\n");
        set_iv(iv);  /* Reset IV for encryption */
        if (!encrypt(plaintext, ciphertext, 32)) {
            return false;
        }
        hexdump("Ciphertext", ciphertext, 32);
        
        /* Decrypt */
        printf("\n--- Decrypting ---\n");
        set_iv(iv);  /* Reset IV for decryption */
        if (!decrypt(ciphertext, decrypted, 32)) {
            return false;
        }
        hexdump("Decrypted ", decrypted, 32);
        
        /* Verify */
        if (memcmp(plaintext, decrypted, 32) == 0) {
            printf("\n=== SUCCESS: Decrypt matches original! ===\n");
            printf("Text: '%.*s'\n", 32, decrypted);
            return true;
        } else {
            printf("\n=== FAILED: Mismatch! ===\n");
            return false;
        }
    }
    
    void cleanup() {
        if (bar5 && bar5 != MAP_FAILED) munmap((void*)bar5, bar5_size);
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
    
    CryptoDevice dev;
    if (!dev.open(pci_addr)) return 1;
    if (!dev.setup_msix()) return 1;
    if (!dev.setup_dma()) return 1;
    
    return dev.test_encrypt_decrypt() ? 0 : 1;
}
