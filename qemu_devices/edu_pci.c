/*
 * QEMU Educational PCI Device with DMA, SR-IOV, and MSI-X
 *
 * Registers (BAR0, 64 bytes) - Simple DMA:
 *   0x00: ID        (RO)  - 0x0ED00001 (PF) or 0x0ED00002 (VF)
 *   0x04: Scratch   (RW)  - Read/write scratch register
 *   0x08: Factorial (RW)  - Write n, read n!
 *   0x0C: Status    (RO)  - Bit 0: ready, Bit 1: DMA busy
 *   0x10: DMA_SRC   (RW)  - DMA source address
 *   0x14: DMA_DST   (RW)  - DMA destination address
 *   0x18: DMA_LEN   (RW)  - DMA transfer length
 *   0x1C: DMA_CMD   (RW)  - Write 1: start DMA, triggers MSI-X on completion
 *   0x20: VF_ID     (RO)  - VF number (0 for PF)
 *   0x24: IRQ_RAISE (WO)  - Write vector number to raise MSI-X interrupt
 *
 * Registers (BAR4, 64 bytes) - Descriptor Ring DMA:
 *   0x00: RING_ID      (RO)  - 0x0ED0RING
 *   0x04: RING_STATUS  (RO)  - Bit 0: enabled, Bit 1: running
 *   0x08: RING_ADDR_LO (RW)  - Ring base address (low 32 bits)
 *   0x0C: RING_ADDR_HI (RW)  - Ring base address (high 32 bits)
 *   0x10: RING_SIZE    (RW)  - Number of descriptors (power of 2, max 256)
 *   0x14: RING_HEAD    (RO)  - Device read pointer (consumer)
 *   0x18: RING_TAIL    (RW)  - Host write pointer (producer) - doorbell
 *   0x1C: RING_CTRL    (RW)  - Bit 0: enable ring
 *
 * Descriptor format (32 bytes each):
 *   0x00: src_addr_lo  (4 bytes)
 *   0x04: src_addr_hi  (4 bytes)
 *   0x08: dst_addr_lo  (4 bytes)
 *   0x0C: dst_addr_hi  (4 bytes)
 *   0x10: length       (4 bytes)
 *   0x14: flags        (4 bytes) - Bit 0: valid, Bit 1: interrupt on completion
 *   0x18: status       (4 bytes) - Written by device: 0=pending, 1=complete, 2=error
 *   0x1C: reserved     (4 bytes)
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_sriov.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "qemu/module.h"
#include "qemu/log.h"

/* Debug flags - set via environment variable EDU_DEBUG
 * 0 = quiet (default)
 * 1 = TLP traces (BAR read/write, DMA, MSI)
 * 2 = + config space traces
 * 3 = + ring DMA traces
 */
static int edu_debug_level = -1;  /* -1 = not initialized */

static int edu_get_debug_level(void)
{
    if (edu_debug_level < 0) {
        const char *env = getenv("EDU_DEBUG");
        edu_debug_level = env ? atoi(env) : 0;
    }
    return edu_debug_level;
}

#define EDU_DBG(level, fmt, ...) \
    do { if (edu_get_debug_level() >= (level)) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)

#define DBG_TLP   1  /* BAR MMIO, DMA, MSI */
#define DBG_CFG   2  /* Config space */
#define DBG_RING  3  /* Ring DMA details */

#define TYPE_EDU_PCI "edu-pci"
#define TYPE_EDU_PCI_VF "edu-pci-vf"
#define EDU_PCI(obj) OBJECT_CHECK(EduPciState, (obj), TYPE_EDU_PCI)
#define EDU_PCI_VF(obj) OBJECT_CHECK(EduPciVfState, (obj), TYPE_EDU_PCI_VF)

#define EDU_VENDOR_ID    0x1234
#define EDU_PF_DEVICE_ID 0xED01
#define EDU_VF_DEVICE_ID 0xED02

#define EDU_NUM_VFS      2
#define SRIOV_CAP_OFFSET 0x100
#define ATS_CAP_OFFSET   0x200   /* Must be in extended config space, after SR-IOV */

#define REG_ID          0x00
#define REG_SCRATCH     0x04
#define REG_FACTORIAL   0x08
#define REG_STATUS      0x0C
#define REG_DMA_SRC     0x10
#define REG_DMA_DST     0x14
#define REG_DMA_LEN     0x18
#define REG_DMA_CMD     0x1C
#define REG_VF_ID       0x20
#define REG_IRQ_RAISE   0x24
#define REG_IRQ_ACK     0x28
#define REG_P2P_ADDR    0x2C   /* P2P: target device BAR address */
#define REG_P2P_DATA    0x30   /* P2P: data to write */
#define REG_P2P_CMD     0x34   /* P2P: write 1 to trigger P2P DMA */
#define REG_ATS_ADDR    0x38   /* ATS: address to translate */
#define REG_ATS_CMD     0x3C   /* ATS: write 1 to request translation */

#define DMA_MAX_LEN     4096
#define EDU_MSIX_VECTORS 4
#define EDU_MSIX_TABLE_BAR 2
#define EDU_MSIX_PBA_BAR   2

/* BAR4: Descriptor Ring DMA registers */
#define REG_RING_ID         0x00
#define REG_RING_STATUS     0x04
#define REG_RING_ADDR_LO    0x08
#define REG_RING_ADDR_HI    0x0C
#define REG_RING_SIZE       0x10
#define REG_RING_HEAD       0x14
#define REG_RING_TAIL       0x18
#define REG_RING_CTRL       0x1C

/* Ring control bits */
#define RING_CTRL_ENABLE    (1 << 0)

/* Descriptor flags */
#define DESC_FLAG_VALID     (1 << 0)
#define DESC_FLAG_INT       (1 << 1)

/* Descriptor status (written by device) */
#define DESC_STATUS_PENDING  0
#define DESC_STATUS_COMPLETE 1
#define DESC_STATUS_ERROR    2

#define DESC_SIZE           32
#define MAX_RING_SIZE       256

/* DMA Descriptor (32 bytes) */
typedef struct {
    uint32_t src_addr_lo;
    uint32_t src_addr_hi;
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
    uint32_t length;
    uint32_t flags;
    uint32_t status;
    uint32_t reserved;
} DmaDescriptor;

/* ============================================================
 * BAR5: AES Crypto Engine
 * ============================================================
 * Registers:
 *   0x00: CRYPTO_ID      (RO)  - 0x0ED0AE55 (magic)
 *   0x04: CRYPTO_STATUS  (RO)  - Bit 0: ready, Bit 1: busy
 *   0x08: CRYPTO_SRC_LO  (RW)  - Source address low
 *   0x0C: CRYPTO_SRC_HI  (RW)  - Source address high
 *   0x10: CRYPTO_DST_LO  (RW)  - Destination address low
 *   0x14: CRYPTO_DST_HI  (RW)  - Destination address high
 *   0x18: CRYPTO_LEN     (RW)  - Data length (must be multiple of 16)
 *   0x1C: CRYPTO_CMD     (RW)  - Bit 0: start, Bit 1: encrypt(1)/decrypt(0)
 *   0x20-0x2F: CRYPTO_KEY (WO) - 128-bit AES key (4 x 32-bit writes)
 *   0x30-0x3F: CRYPTO_IV  (RW) - 128-bit IV for CBC mode
 */

#define REG_CRYPTO_ID       0x00
#define REG_CRYPTO_STATUS   0x04
#define REG_CRYPTO_SRC_LO   0x08
#define REG_CRYPTO_SRC_HI   0x0C
#define REG_CRYPTO_DST_LO   0x10
#define REG_CRYPTO_DST_HI   0x14
#define REG_CRYPTO_LEN      0x18
#define REG_CRYPTO_CMD      0x1C
#define REG_CRYPTO_KEY      0x20  /* 0x20-0x2F: 16 bytes */
#define REG_CRYPTO_IV       0x30  /* 0x30-0x3F: 16 bytes */

#define CRYPTO_CMD_START    (1 << 0)
#define CRYPTO_CMD_ENCRYPT  (1 << 1)  /* 1=encrypt, 0=decrypt */

#define AES_BLOCK_SIZE      16
#define AES_KEY_SIZE        16  /* AES-128 */

/* AES S-box */
static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* AES inverse S-box */
static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Rcon for key expansion */
static const uint8_t rcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/* Multiply by 2 in GF(2^8) */
static inline uint8_t xtime(uint8_t x) {
    return (x << 1) ^ ((x & 0x80) ? 0x1b : 0x00);
}

/* AES key expansion */
static void aes_key_expand(const uint8_t *key, uint8_t *rk) {
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, &rk[(i-1)*4], 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ rcon[i/4-1];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        for (int j = 0; j < 4; j++)
            rk[i*4+j] = rk[(i-4)*4+j] ^ t[j];
    }
}

/* AES encrypt one block */
static void aes_encrypt_block(const uint8_t *rk, const uint8_t *in, uint8_t *out) {
    uint8_t s[16];
    memcpy(s, in, 16);
    
    /* AddRoundKey(0) */
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    
    for (int r = 1; r < 10; r++) {
        /* SubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
        /* ShiftRows */
        uint8_t t;
        t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
        /* MixColumns */
        for (int i = 0; i < 4; i++) {
            uint8_t *c = &s[i*4];
            uint8_t a=c[0], b=c[1], cc=c[2], d=c[3];
            c[0] = xtime(a)^xtime(b)^b^cc^d;
            c[1] = a^xtime(b)^xtime(cc)^cc^d;
            c[2] = a^b^xtime(cc)^xtime(d)^d;
            c[3] = xtime(a)^a^b^cc^xtime(d);
        }
        /* AddRoundKey */
        for (int i = 0; i < 16; i++) s[i] ^= rk[r*16+i];
    }
    
    /* Final round (no MixColumns) */
    for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
    uint8_t t;
    t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
    t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
    for (int i = 0; i < 16; i++) s[i] ^= rk[160+i];
    
    memcpy(out, s, 16);
}

/* AES decrypt one block */
static void aes_decrypt_block(const uint8_t *rk, const uint8_t *in, uint8_t *out) {
    uint8_t s[16], t;
    memcpy(s, in, 16);
    
    /* Initial round: AddRoundKey only */
    for (int i = 0; i < 16; i++) s[i] ^= rk[160+i];
    
    for (int r = 9; r >= 1; r--) {
        /* InvShiftRows */
        t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
        /* InvSubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];
        /* AddRoundKey */
        for (int i = 0; i < 16; i++) s[i] ^= rk[r*16+i];
        /* InvMixColumns */
        for (int i = 0; i < 4; i++) {
            uint8_t *c = &s[i*4];
            uint8_t a=c[0], b=c[1], cc=c[2], d=c[3];
            uint8_t x2a=xtime(a), x2b=xtime(b), x2c=xtime(cc), x2d=xtime(d);
            uint8_t x4a=xtime(x2a), x4b=xtime(x2b), x4c=xtime(x2c), x4d=xtime(x2d);
            uint8_t x8a=xtime(x4a), x8b=xtime(x4b), x8c=xtime(x4c), x8d=xtime(x4d);
            /* 0e=8+4+2, 0b=8+2+1, 0d=8+4+1, 09=8+1 */
            c[0] = (x8a^x4a^x2a) ^ (x8b^x2b^b) ^ (x8c^x4c^cc) ^ (x8d^d);
            c[1] = (x8a^a) ^ (x8b^x4b^x2b) ^ (x8c^x2c^cc) ^ (x8d^x4d^d);
            c[2] = (x8a^x4a^a) ^ (x8b^b) ^ (x8c^x4c^x2c) ^ (x8d^x2d^d);
            c[3] = (x8a^x2a^a) ^ (x8b^x4b^b) ^ (x8c^cc) ^ (x8d^x4d^x2d);
        }
    }
    
    /* Final round: InvShiftRows + InvSubBytes + AddRoundKey(0) */
    t=s[13]; s[13]=s[9]; s[9]=s[5]; s[5]=s[1]; s[1]=t;
    t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
    t=s[3]; s[3]=s[7]; s[7]=s[11]; s[11]=s[15]; s[15]=t;
    for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    
    memcpy(out, s, 16);
}

/* AES-CBC encrypt */
static void aes_cbc_encrypt(const uint8_t *rk, const uint8_t *iv,
                            const uint8_t *in, uint8_t *out, uint32_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (uint32_t i = 0; i < len; i += 16) {
        uint8_t block[16];
        for (int j = 0; j < 16; j++) block[j] = in[i+j] ^ prev[j];
        aes_encrypt_block(rk, block, &out[i]);
        memcpy(prev, &out[i], 16);
    }
}

/* AES-CBC decrypt */
static void aes_cbc_decrypt(const uint8_t *rk, const uint8_t *iv,
                            const uint8_t *in, uint8_t *out, uint32_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (uint32_t i = 0; i < len; i += 16) {
        uint8_t block[16];
        aes_decrypt_block(rk, &in[i], block);
        for (int j = 0; j < 16; j++) out[i+j] = block[j] ^ prev[j];
        memcpy(prev, &in[i], 16);
    }
}

/* PF State */
typedef struct {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion ring_mmio;      /* BAR4: descriptor ring DMA */
    MemoryRegion crypto_mmio;    /* BAR5: AES crypto engine */
    uint32_t scratch;
    uint32_t factorial_out;
    uint64_t dma_src;
    uint64_t dma_dst;
    uint32_t dma_len;
    uint32_t dma_busy;
    uint64_t p2p_addr;   /* P2P target address */
    uint32_t p2p_data;   /* P2P data to write */
    uint64_t ats_addr;   /* ATS address to translate */
    
    /* Descriptor ring DMA state */
    uint64_t ring_addr;         /* Base address of descriptor ring */
    uint32_t ring_size;         /* Number of descriptors */
    uint32_t ring_head;         /* Device read pointer */
    uint32_t ring_tail;         /* Host write pointer */
    uint32_t ring_enabled;      /* Ring is active */
    
    /* AES Crypto engine state */
    uint64_t crypto_src;        /* Source address */
    uint64_t crypto_dst;        /* Destination address */
    uint32_t crypto_len;        /* Data length */
    uint32_t crypto_busy;       /* Operation in progress */
    uint8_t  crypto_key[16];    /* AES-128 key */
    uint8_t  crypto_iv[16];     /* IV for CBC mode */
    uint8_t  round_keys[176];   /* Expanded key schedule */
} EduPciState;

/* VF State */
typedef struct {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t scratch;
    uint32_t factorial_out;
    uint64_t dma_src;
    uint64_t dma_dst;
    uint32_t dma_len;
    uint32_t dma_busy;
} EduPciVfState;

static uint32_t factorial(uint32_t n)
{
    uint32_t result = 1;
    for (uint32_t i = 2; i <= n && i < 13; i++) {
        result *= i;
    }
    return result;
}

/* PF read */
static uint64_t edu_pci_read(void *opaque, hwaddr addr, unsigned size)
{
    EduPciState *s = opaque;
    uint64_t val;
    uint8_t first_be = (1 << size) - 1;  /* Byte enables: size=4 -> 0xF */
    
    switch (addr) {
    case REG_ID:        val = 0x0ED00001; break;
    case REG_SCRATCH:   val = s->scratch; break;
    case REG_FACTORIAL: val = s->factorial_out; break;
    case REG_STATUS:    val = 1 | (s->dma_busy << 1); break;
    case REG_DMA_SRC:   val = s->dma_src; break;
    case REG_DMA_DST:   val = s->dma_dst; break;
    case REG_DMA_LEN:   val = s->dma_len; break;
    case REG_DMA_CMD:   val = s->dma_busy; break;
    case REG_VF_ID:     val = 0; break;
    case REG_P2P_ADDR:  val = s->p2p_addr; break;
    case REG_P2P_DATA:  val = s->p2p_data; break;
    case REG_ATS_ADDR:  val = s->ats_addr; break;
    default:            val = 0xFFFFFFFF; break;
    }
    
    EDU_DBG(DBG_TLP, "[TLP] MRd%d: BAR0+0x%02lx len=%d BE=0x%x => CplD: data=0x%0*lx\n",
            (addr > 0xFFFFFFFF) ? 64 : 32,
            (unsigned long)addr, size, first_be,
            size * 2, (unsigned long)val);
    return val;
}

/* PF write */
static void edu_pci_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EduPciState *s = opaque;
    uint8_t first_be = (1 << size) - 1;
    
    EDU_DBG(DBG_TLP, "[TLP] MWr%d: BAR0+0x%02lx len=%d BE=0x%x data=0x%0*lx\n",
            (addr > 0xFFFFFFFF) ? 64 : 32,
            (unsigned long)addr, size, first_be,
            size * 2, (unsigned long)val);
    
    switch (addr) {
    case REG_SCRATCH:   s->scratch = val; break;
    case REG_FACTORIAL: s->factorial_out = factorial(val); break;
    case REG_DMA_SRC:   s->dma_src = val; break;
    case REG_DMA_DST:   s->dma_dst = val; break;
    case REG_DMA_LEN:   s->dma_len = val; break;
    case REG_DMA_CMD:
        if (val == 1 && !s->dma_busy) {
            uint8_t buf[DMA_MAX_LEN];
            uint32_t len = s->dma_len > DMA_MAX_LEN ? DMA_MAX_LEN : s->dma_len;
            uint32_t dw_len = (len + 3) / 4;  /* Length in DWORDs */
            MemTxResult res;
            s->dma_busy = 1;
            
            EDU_DBG(DBG_TLP, "[TLP] DMA MRd%d: iova=0x%lx len=%uDW (%u bytes) [device->host read]\n",
                   (s->dma_src > 0xFFFFFFFF) ? 64 : 32,
                   (unsigned long)s->dma_src, dw_len, len);
            res = pci_dma_read(&s->parent_obj, s->dma_src, buf, len);
            EDU_DBG(DBG_TLP, "[TLP]     CplD: status=%s data[0:3]=%02x %02x %02x %02x\n",
                   res == MEMTX_OK ? "SC" : "UR",
                   len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0,
                   len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0);
            
            EDU_DBG(DBG_TLP, "[TLP] DMA MWr%d: iova=0x%lx len=%uDW (%u bytes) [device->host write]\n",
                   (s->dma_dst > 0xFFFFFFFF) ? 64 : 32,
                   (unsigned long)s->dma_dst, dw_len, len);
            pci_dma_write(&s->parent_obj, s->dma_dst, buf, len);
            
            s->dma_busy = 0;
            /* Send MSI-X interrupt on DMA completion (vector 0) */
            if (msix_enabled(&s->parent_obj)) {
                MSIMessage msg = msix_get_message(&s->parent_obj, 0);
                EDU_DBG(DBG_TLP, "[TLP] MSI: MWr32 addr=0x%lx data=0x%x (vector=0)\n",
                       (unsigned long)msg.address, msg.data);
                msix_notify(&s->parent_obj, 0);
            }
        }
        break;
    case REG_IRQ_RAISE:
        /* Manually raise interrupt for testing */
        if (msix_enabled(&s->parent_obj) && val < EDU_MSIX_VECTORS) {
            MSIMessage msg = msix_get_message(&s->parent_obj, val);
            EDU_DBG(DBG_TLP, "[TLP] MSI: MWr32 addr=0x%lx data=0x%x (vector=%lu)\n",
                   (unsigned long)msg.address, msg.data, (unsigned long)val);
            msix_notify(&s->parent_obj, val);
        } else if (!msix_enabled(&s->parent_obj)) {
            /* Legacy INTx */
            pci_irq_assert(&s->parent_obj);
        }
        break;
    case REG_IRQ_ACK:
        /* Acknowledge/deassert INTx */
        EDU_DBG(DBG_TLP, "EDU-PF: IRQ_ACK - deasserting INTx\n");
        pci_irq_deassert(&s->parent_obj);
        break;
    case REG_P2P_ADDR:
        s->p2p_addr = val;
        break;
    case REG_P2P_DATA:
        s->p2p_data = val;
        break;
    case REG_P2P_CMD:
        if (val == 1) {
            /* P2P DMA: write p2p_data to p2p_addr (another device's BAR) */
            EDU_DBG(DBG_TLP, "[TLP] P2P MWr%d: addr=0x%lx data=0x%x [device->device write]\n",
                   (s->p2p_addr > 0xFFFFFFFF) ? 64 : 32,
                   (unsigned long)s->p2p_addr, s->p2p_data);
            pci_dma_write(&s->parent_obj, s->p2p_addr, &s->p2p_data, 4);
            EDU_DBG(DBG_TLP, "[TLP] P2P complete!\n");
        }
        break;
    case REG_ATS_ADDR:
        s->ats_addr = val;
        break;
    case REG_ATS_CMD:
        if (val == 1) {
            /* ATS: Request translation for ats_addr 
             * Note: We simulate ATS without requiring the PCIe ATS capability.
             * In real hardware, this would use pci_ats_request_translation().
             * Here we directly call the IOMMU translate function.
             */
            EDU_DBG(DBG_TLP, "[TLP] ATS TransReq: iova=0x%lx\n", 
                   (unsigned long)s->ats_addr);
            
            /* Do a DMA read of 0 bytes just to trigger IOMMU translation */
            uint8_t dummy;
            MemTxResult res = pci_dma_read(&s->parent_obj, s->ats_addr, &dummy, 0);
            
            if (res == MEMTX_OK) {
                EDU_DBG(DBG_TLP, "[TLP] ATS TransCmpl: iova=0x%lx -> translation OK\n",
                       (unsigned long)s->ats_addr);
            } else {
                EDU_DBG(DBG_TLP, "[TLP] ATS TransCmpl: iova=0x%lx -> FAILED (unmapped)\n",
                       (unsigned long)s->ats_addr);
            }
        }
        break;
    }
}

/* Process one descriptor from the ring */
static void edu_ring_process_one(EduPciState *s)
{
    DmaDescriptor desc;
    uint64_t desc_addr;
    uint64_t src_addr, dst_addr;
    uint8_t buf[DMA_MAX_LEN];
    uint32_t len;
    MemTxResult res;
    
    /* Calculate descriptor address */
    desc_addr = s->ring_addr + (s->ring_head * DESC_SIZE);
    
    /* Read descriptor from guest memory */
    EDU_DBG(DBG_RING, "[RING] Reading descriptor %u at iova=0x%lx\n",
            s->ring_head, (unsigned long)desc_addr);
    res = pci_dma_read(&s->parent_obj, desc_addr, &desc, sizeof(desc));
    if (res != MEMTX_OK) {
        EDU_DBG(DBG_RING, "[RING] ERROR: Failed to read descriptor\n");
        return;
    }
    
    /* Check if descriptor is valid */
    if (!(desc.flags & DESC_FLAG_VALID)) {
        EDU_DBG(DBG_RING, "[RING] Descriptor %u not valid, stopping\n", s->ring_head);
        return;
    }
    
    /* Build 64-bit addresses */
    src_addr = ((uint64_t)desc.src_addr_hi << 32) | desc.src_addr_lo;
    dst_addr = ((uint64_t)desc.dst_addr_hi << 32) | desc.dst_addr_lo;
    len = desc.length > DMA_MAX_LEN ? DMA_MAX_LEN : desc.length;
    
    EDU_DBG(DBG_RING, "[RING] Desc %u: src=0x%lx dst=0x%lx len=%u flags=0x%x\n",
            s->ring_head, (unsigned long)src_addr, (unsigned long)dst_addr,
            len, desc.flags);
    
    /* Perform DMA */
    EDU_DBG(DBG_TLP, "[TLP] Ring DMA MRd: iova=0x%lx len=%u\n",
            (unsigned long)src_addr, len);
    res = pci_dma_read(&s->parent_obj, src_addr, buf, len);
    if (res != MEMTX_OK) {
        EDU_DBG(DBG_RING, "[RING] ERROR: DMA read failed\n");
        desc.status = DESC_STATUS_ERROR;
    } else {
        EDU_DBG(DBG_TLP, "[TLP] Ring DMA MWr: iova=0x%lx len=%u\n",
                (unsigned long)dst_addr, len);
        pci_dma_write(&s->parent_obj, dst_addr, buf, len);
        desc.status = DESC_STATUS_COMPLETE;
    }
    
    /* Write back descriptor status */
    pci_dma_write(&s->parent_obj, desc_addr + offsetof(DmaDescriptor, status),
                  &desc.status, sizeof(desc.status));
    
    /* Advance head pointer (wrap around) */
    s->ring_head = (s->ring_head + 1) % s->ring_size;
    
    /* Send interrupt if requested */
    if ((desc.flags & DESC_FLAG_INT) && msix_enabled(&s->parent_obj)) {
        EDU_DBG(DBG_RING, "[RING] Descriptor requested interrupt, sending MSI-X vector 1\n");
        msix_notify(&s->parent_obj, 1);  /* Use vector 1 for ring DMA */
    }
}

/* Process all pending descriptors */
static void edu_ring_process(EduPciState *s)
{
    uint32_t count = 0;
    
    if (!s->ring_enabled || s->ring_size == 0) {
        return;
    }
    
    EDU_DBG(DBG_RING, "[RING] Processing: head=%u tail=%u\n", s->ring_head, s->ring_tail);
    
    /* Process while head != tail */
    while (s->ring_head != s->ring_tail && count < s->ring_size) {
        edu_ring_process_one(s);
        count++;
    }
    
    EDU_DBG(DBG_RING, "[RING] Processed %u descriptors, new head=%u\n", count, s->ring_head);
}

/* BAR4: Ring DMA read */
static uint64_t edu_ring_read(void *opaque, hwaddr addr, unsigned size)
{
    EduPciState *s = opaque;
    uint64_t val;
    
    switch (addr) {
    case REG_RING_ID:       val = 0x0ED0CAFE; break;  /* Magic ID */
    case REG_RING_STATUS:   val = s->ring_enabled | ((s->ring_head != s->ring_tail) << 1); break;
    case REG_RING_ADDR_LO:  val = s->ring_addr & 0xFFFFFFFF; break;
    case REG_RING_ADDR_HI:  val = s->ring_addr >> 32; break;
    case REG_RING_SIZE:     val = s->ring_size; break;
    case REG_RING_HEAD:     val = s->ring_head; break;
    case REG_RING_TAIL:     val = s->ring_tail; break;
    case REG_RING_CTRL:     val = s->ring_enabled; break;
    default:                val = 0xFFFFFFFF; break;
    }
    
    EDU_DBG(DBG_TLP, "[TLP] MRd32: BAR4+0x%02lx len=%d => CplD: 0x%08lx\n",
            (unsigned long)addr, size, (unsigned long)val);
    return val;
}

/* BAR4: Ring DMA write */
static void edu_ring_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EduPciState *s = opaque;
    
    EDU_DBG(DBG_TLP, "[TLP] MWr32: BAR4+0x%02lx len=%d data=0x%08lx\n",
            (unsigned long)addr, size, (unsigned long)val);
    
    switch (addr) {
    case REG_RING_ADDR_LO:
        s->ring_addr = (s->ring_addr & 0xFFFFFFFF00000000ULL) | val;
        EDU_DBG(DBG_RING, "[RING] Ring address low = 0x%x, full = 0x%lx\n",
                (uint32_t)val, (unsigned long)s->ring_addr);
        break;
    case REG_RING_ADDR_HI:
        s->ring_addr = (s->ring_addr & 0xFFFFFFFF) | ((uint64_t)val << 32);
        EDU_DBG(DBG_RING, "[RING] Ring address high = 0x%x, full = 0x%lx\n",
                (uint32_t)val, (unsigned long)s->ring_addr);
        break;
    case REG_RING_SIZE:
        if (val > 0 && val <= MAX_RING_SIZE && (val & (val - 1)) == 0) {
            s->ring_size = val;
            s->ring_head = 0;
            s->ring_tail = 0;
            EDU_DBG(DBG_RING, "[RING] Ring size set to %u descriptors\n", s->ring_size);
        } else {
            EDU_DBG(DBG_RING, "[RING] Invalid ring size %lu (must be power of 2, max %d)\n",
                    (unsigned long)val, MAX_RING_SIZE);
        }
        break;
    case REG_RING_TAIL:
        /* Doorbell! Host is telling us there are new descriptors */
        s->ring_tail = val % s->ring_size;
        EDU_DBG(DBG_RING, "[RING] Doorbell! tail=%u (head=%u)\n", s->ring_tail, s->ring_head);
        if (s->ring_enabled) {
            edu_ring_process(s);
        }
        break;
    case REG_RING_CTRL:
        s->ring_enabled = val & RING_CTRL_ENABLE;
        EDU_DBG(DBG_RING, "[RING] Ring %s\n", s->ring_enabled ? "ENABLED" : "DISABLED");
        if (s->ring_enabled && s->ring_head != s->ring_tail) {
            edu_ring_process(s);
        }
        break;
    }
}

static const MemoryRegionOps edu_ring_ops = {
    .read = edu_ring_read,
    .write = edu_ring_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ============================================================
 * BAR5: AES Crypto Engine read/write handlers
 * ============================================================ */

/* BAR5: Crypto read */
static uint64_t edu_crypto_read(void *opaque, hwaddr addr, unsigned size)
{
    EduPciState *s = opaque;
    uint64_t val;
    
    switch (addr) {
    case REG_CRYPTO_ID:      val = 0x0ED0AE55; break;  /* Magic: EDU AES */
    case REG_CRYPTO_STATUS:  val = 1 | (s->crypto_busy << 1); break;
    case REG_CRYPTO_SRC_LO:  val = s->crypto_src & 0xFFFFFFFF; break;
    case REG_CRYPTO_SRC_HI:  val = s->crypto_src >> 32; break;
    case REG_CRYPTO_DST_LO:  val = s->crypto_dst & 0xFFFFFFFF; break;
    case REG_CRYPTO_DST_HI:  val = s->crypto_dst >> 32; break;
    case REG_CRYPTO_LEN:     val = s->crypto_len; break;
    case REG_CRYPTO_CMD:     val = s->crypto_busy; break;
    default:
        /* IV is readable (0x30-0x3F) */
        if (addr >= REG_CRYPTO_IV && addr < REG_CRYPTO_IV + 16) {
            int idx = addr - REG_CRYPTO_IV;
            memcpy(&val, &s->crypto_iv[idx], size);
        } else {
            val = 0xFFFFFFFF;
        }
        break;
    }
    
    EDU_DBG(DBG_TLP, "[TLP] MRd32: BAR5+0x%02lx len=%d => CplD: 0x%08lx\n",
            (unsigned long)addr, size, (unsigned long)val);
    return val;
}

/* BAR5: Crypto write */
static void edu_crypto_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EduPciState *s = opaque;
    
    EDU_DBG(DBG_TLP, "[TLP] MWr32: BAR5+0x%02lx len=%d data=0x%08lx\n",
            (unsigned long)addr, size, (unsigned long)val);
    
    switch (addr) {
    case REG_CRYPTO_SRC_LO:
        s->crypto_src = (s->crypto_src & 0xFFFFFFFF00000000ULL) | val;
        break;
    case REG_CRYPTO_SRC_HI:
        s->crypto_src = (s->crypto_src & 0xFFFFFFFF) | ((uint64_t)val << 32);
        break;
    case REG_CRYPTO_DST_LO:
        s->crypto_dst = (s->crypto_dst & 0xFFFFFFFF00000000ULL) | val;
        break;
    case REG_CRYPTO_DST_HI:
        s->crypto_dst = (s->crypto_dst & 0xFFFFFFFF) | ((uint64_t)val << 32);
        break;
    case REG_CRYPTO_LEN:
        s->crypto_len = val;
        break;
    case REG_CRYPTO_CMD:
        if ((val & CRYPTO_CMD_START) && !s->crypto_busy) {
            /* Start crypto operation */
            uint8_t buf[DMA_MAX_LEN];
            uint8_t out[DMA_MAX_LEN];
            uint32_t len = s->crypto_len;
            int encrypt = (val & CRYPTO_CMD_ENCRYPT) ? 1 : 0;
            
            if (len > DMA_MAX_LEN) len = DMA_MAX_LEN;
            if (len % AES_BLOCK_SIZE != 0) {
                EDU_DBG(DBG_TLP, "[CRYPTO] ERROR: length %u not multiple of 16\n", len);
                break;
            }
            
            s->crypto_busy = 1;
            
            EDU_DBG(DBG_TLP, "[CRYPTO] %s: src=0x%lx dst=0x%lx len=%u\n",
                    encrypt ? "ENCRYPT" : "DECRYPT",
                    (unsigned long)s->crypto_src, (unsigned long)s->crypto_dst, len);
            
            /* Read input from guest memory */
            pci_dma_read(&s->parent_obj, s->crypto_src, buf, len);
            
            /* Perform AES-CBC */
            if (encrypt) {
                aes_cbc_encrypt(s->round_keys, s->crypto_iv, buf, out, len);
            } else {
                aes_cbc_decrypt(s->round_keys, s->crypto_iv, buf, out, len);
            }
            
            /* Write result to guest memory */
            pci_dma_write(&s->parent_obj, s->crypto_dst, out, len);
            
            s->crypto_busy = 0;
            
            /* Send MSI-X interrupt (vector 2 for crypto) */
            if (msix_enabled(&s->parent_obj)) {
                EDU_DBG(DBG_TLP, "[CRYPTO] Complete, sending MSI-X vector 2\n");
                msix_notify(&s->parent_obj, 2);
            }
        }
        break;
    default:
        /* Key write (0x20-0x2F) */
        if (addr >= REG_CRYPTO_KEY && addr < REG_CRYPTO_KEY + 16) {
            int idx = addr - REG_CRYPTO_KEY;
            memcpy(&s->crypto_key[idx], &val, size);
            EDU_DBG(DBG_TLP, "[CRYPTO] Key[%d-%d] written\n", idx, idx + size - 1);
            /* Expand key when fully written */
            if (idx + size >= 16) {
                aes_key_expand(s->crypto_key, s->round_keys);
                EDU_DBG(DBG_TLP, "[CRYPTO] Key expansion complete\n");
            }
        }
        /* IV write (0x30-0x3F) */
        else if (addr >= REG_CRYPTO_IV && addr < REG_CRYPTO_IV + 16) {
            int idx = addr - REG_CRYPTO_IV;
            memcpy(&s->crypto_iv[idx], &val, size);
            EDU_DBG(DBG_TLP, "[CRYPTO] IV[%d-%d] written\n", idx, idx + size - 1);
        }
        break;
    }
}

static const MemoryRegionOps edu_crypto_ops = {
    .read = edu_crypto_read,
    .write = edu_crypto_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* VF read */
static uint64_t edu_vf_read(void *opaque, hwaddr addr, unsigned size)
{
    EduPciVfState *s = opaque;
    uint16_t vf_num = pcie_sriov_vf_number(&s->parent_obj);
    switch (addr) {
    case REG_ID:        return 0x0ED00002;
    case REG_SCRATCH:   return s->scratch;
    case REG_FACTORIAL: return s->factorial_out;
    case REG_STATUS:    return 1 | (s->dma_busy << 1);
    case REG_DMA_SRC:   return s->dma_src;
    case REG_DMA_DST:   return s->dma_dst;
    case REG_DMA_LEN:   return s->dma_len;
    case REG_DMA_CMD:   return s->dma_busy;
    case REG_VF_ID:     return vf_num + 1;
    default:            return 0xFFFFFFFF;
    }
}

/* VF write */
static void edu_vf_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EduPciVfState *s = opaque;
    switch (addr) {
    case REG_SCRATCH:   s->scratch = val; break;
    case REG_FACTORIAL: s->factorial_out = factorial(val); break;
    case REG_DMA_SRC:   s->dma_src = val; break;
    case REG_DMA_DST:   s->dma_dst = val; break;
    case REG_DMA_LEN:   s->dma_len = val; break;
    case REG_DMA_CMD:
        if (val == 1 && !s->dma_busy) {
            uint8_t buf[DMA_MAX_LEN];
            uint32_t len = s->dma_len > DMA_MAX_LEN ? DMA_MAX_LEN : s->dma_len;
            s->dma_busy = 1;
            pci_dma_read(&s->parent_obj, s->dma_src, buf, len);
            pci_dma_write(&s->parent_obj, s->dma_dst, buf, len);
            s->dma_busy = 0;
        }
        break;
    }
}

/* Config space register names */
static const char *cfg_reg_name(uint32_t addr)
{
    switch (addr) {
    case 0x00: return "VID/DID";
    case 0x04: return "CMD/STS";
    case 0x08: return "REV/CLASS";
    case 0x0C: return "BIST/HDR/LAT/CLS";
    case 0x10: return "BAR0";
    case 0x14: return "BAR1";
    case 0x18: return "BAR2";
    case 0x3C: return "INT_LINE/PIN";
    default:
        if (addr >= 0x80 && addr < 0x100) return "PCIe_CAP";
        if (addr >= 0x100) return "EXT_CFG";
        return "CFG";
    }
}

/* Config space read with TLP trace */
static uint32_t edu_pci_config_read(PCIDevice *pdev, uint32_t addr, int len)
{
    uint32_t val = pci_default_read_config(pdev, addr, len);
    EDU_DBG(DBG_CFG, "[TLP] CfgRd0: %s (0x%02x) len=%d => CplD: 0x%0*x\n",
            cfg_reg_name(addr), addr, len, len * 2, val);
    return val;
}

/* Config space write with TLP trace */
static void edu_pci_config_write(PCIDevice *pdev, uint32_t addr, uint32_t val, int len)
{
    EDU_DBG(DBG_CFG, "[TLP] CfgWr0: %s (0x%02x) len=%d data=0x%0*x",
            cfg_reg_name(addr), addr, len, len * 2, val);

    /* Check for FLR */
    if (addr == pdev->exp.exp_cap + PCI_EXP_DEVCTL) {
        if (val & PCI_EXP_DEVCTL_BCR_FLR) {
            EDU_DBG(DBG_CFG, " [FLR triggered!]");
        }
    }
    EDU_DBG(DBG_CFG, "\n");

    pci_default_write_config(pdev, addr, val, len);
    pcie_cap_flr_write_config(pdev, addr, val, len);
}

static const MemoryRegionOps edu_pci_ops = {
    .read = edu_pci_read,
    .write = edu_pci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps edu_vf_ops = {
    .read = edu_vf_read,
    .write = edu_vf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* VF realize */
static void edu_vf_realize(PCIDevice *pdev, Error **errp)
{
    EduPciVfState *s = EDU_PCI_VF(pdev);

    memory_region_init_io(&s->mmio, OBJECT(s), &edu_vf_ops, s,
                          "edu-vf-mmio", 64);
    pci_register_bar(pdev, 0,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
        &s->mmio);
}

/* PF realize */
static void edu_pci_realize(PCIDevice *pdev, Error **errp)
{
    EduPciState *s = EDU_PCI(pdev);

    /* BAR0: Simple register-based DMA */
    memory_region_init_io(&s->mmio, OBJECT(s), &edu_pci_ops, s,
                          "edu-pci-mmio", 4096);
    pci_register_bar(pdev, 0,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
        &s->mmio);

    /* BAR4: Descriptor ring DMA engine (32-bit to leave room for BAR5) */
    memory_region_init_io(&s->ring_mmio, OBJECT(s), &edu_ring_ops, s,
                          "edu-pci-ring", 4096);
    pci_register_bar(pdev, 4,
        PCI_BASE_ADDRESS_SPACE_MEMORY,
        &s->ring_mmio);

    /* BAR5: AES Crypto engine (32-bit) */
    memory_region_init_io(&s->crypto_mmio, OBJECT(s), &edu_crypto_ops, s,
                          "edu-pci-crypto", 4096);
    pci_register_bar(pdev, 5,
        PCI_BASE_ADDRESS_SPACE_MEMORY,
        &s->crypto_mmio);

    /* Initialize MSI-X with 4 vectors - uses BAR2 exclusively */
    if (msix_init_exclusive_bar(pdev, EDU_MSIX_VECTORS, EDU_MSIX_TABLE_BAR, errp)) {
        return;
    }
    /* Mark all vectors as usable so msix_notify works when guest programs them */
    for (int i = 0; i < EDU_MSIX_VECTORS; i++) {
        msix_vector_use(pdev, i);
    }

    /* Initialize PCIe capability (required for SR-IOV) */
    if (pcie_endpoint_cap_init(pdev, 0x80) < 0) {
        error_setg(errp, "Failed to init PCIe cap");
        return;
    }

    /* Note: ATS capability skipped to avoid config space conflicts with SR-IOV.
     * We still simulate ATS translation requests via registers. */

    /* Initialize SR-IOV capability */
    if (!pcie_sriov_pf_init(pdev, SRIOV_CAP_OFFSET, TYPE_EDU_PCI_VF,
                            EDU_VF_DEVICE_ID, EDU_NUM_VFS, EDU_NUM_VFS,
                            1, 1, errp)) {
        error_prepend(errp, "Failed to init SR-IOV: ");
        return;
    }

    /* Setup VF BAR0 */
    pcie_sriov_pf_init_vf_bar(pdev, 0,
        PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, 64);
}

static void edu_pci_exit(PCIDevice *pdev)
{
    msix_uninit_exclusive_bar(pdev);
    pcie_sriov_pf_exit(pdev);
}

/* PF class init */
static void edu_pci_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = edu_pci_realize;
    k->exit = edu_pci_exit;
    k->config_read = edu_pci_config_read;
    k->config_write = edu_pci_config_write;
    k->vendor_id = EDU_VENDOR_ID;
    k->device_id = EDU_PF_DEVICE_ID;
    k->revision = 0x03;
    k->class_id = PCI_CLASS_MEMORY_OTHER;
}

/* VF class init */
static void edu_vf_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = edu_vf_realize;
    k->vendor_id = EDU_VENDOR_ID;
    k->device_id = EDU_VF_DEVICE_ID;
    k->revision = 0x03;
    k->class_id = PCI_CLASS_MEMORY_OTHER;
}

static const TypeInfo edu_pci_info = {
    .name          = TYPE_EDU_PCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EduPciState),
    .class_init    = edu_pci_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static const TypeInfo edu_vf_info = {
    .name          = TYPE_EDU_PCI_VF,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EduPciVfState),
    .class_init    = edu_vf_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void edu_pci_register_types(void)
{
    type_register_static(&edu_pci_info);
    type_register_static(&edu_vf_info);
}

type_init(edu_pci_register_types)
