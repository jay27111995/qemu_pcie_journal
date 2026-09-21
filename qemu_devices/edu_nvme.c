/*
 * Educational NVMe Controller
 * Minimal implementation to learn NVMe concepts
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "qemu/log.h"
#include "qapi/error.h"

#define TYPE_EDU_NVME "edu-nvme"
#define EDU_NVME(obj) OBJECT_CHECK(EduNvmeState, (obj), TYPE_EDU_NVME)

/* NVMe register offsets (BAR0) */
#define NVME_CAP        0x00    /* Controller Capabilities */
#define NVME_VS         0x08    /* Version */
#define NVME_INTMS      0x0C    /* Interrupt Mask Set */
#define NVME_INTMC      0x10    /* Interrupt Mask Clear */
#define NVME_CC         0x14    /* Controller Configuration */
#define NVME_CSTS       0x1C    /* Controller Status */
#define NVME_AQA        0x24    /* Admin Queue Attributes */
#define NVME_ASQ        0x28    /* Admin Submission Queue Base */
#define NVME_ACQ        0x30    /* Admin Completion Queue Base */
#define NVME_SQ0TDBL    0x1000  /* Submission Queue 0 Tail Doorbell */
#define NVME_CQ0HDBL    0x1004  /* Completion Queue 0 Head Doorbell */

/* Controller Configuration bits */
#define NVME_CC_EN      (1 << 0)

/* Controller Status bits */
#define NVME_CSTS_RDY   (1 << 0)

/* NVMe opcodes */
#define NVME_CMD_IDENTIFY   0x06
#define NVME_CMD_READ       0x02
#define NVME_CMD_WRITE      0x01

/* Storage size: 1MB virtual SSD */
#define STORAGE_SIZE    (1024 * 1024)
#define BLOCK_SIZE      512
#define NUM_BLOCKS      (STORAGE_SIZE / BLOCK_SIZE)

/* Queue sizes */
#define QUEUE_SIZE      64

typedef struct NvmeCmd {
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
} NvmeCmd;

typedef struct NvmeCqe {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} NvmeCqe;

typedef struct EduNvmeState {
    PCIDevice parent_obj;
    MemoryRegion mmio;

    /* Registers */
    uint32_t cc;            /* Controller Configuration */
    uint32_t csts;          /* Controller Status */
    uint32_t aqa;           /* Admin Queue Attributes */
    uint64_t asq;           /* Admin SQ base address */
    uint64_t acq;           /* Admin CQ base address */

    /* Queue state */
    uint16_t sq_tail;       /* Where host wrote last command */
    uint16_t sq_head;       /* Where we read last command */
    uint16_t cq_tail;       /* Where we wrote last completion */
    uint16_t cq_head;       /* Where host read last completion */
    uint8_t cq_phase;       /* Phase bit for completions */

    /* Virtual SSD storage */
    uint8_t *storage;
} EduNvmeState;

static void edu_nvme_process_cmd(EduNvmeState *s, NvmeCmd *cmd)
{
    NvmeCqe cqe = {0};
    uint64_t lba, prp1;
    uint32_t nlb;
    MemTxResult res;

    cqe.cid = cmd->cid;
    cqe.sq_id = 0;
    cqe.sq_head = s->sq_head;

    switch (cmd->opcode) {
    case NVME_CMD_IDENTIFY:
        /* Return fake identify data */
        if (cmd->prp1) {
            uint8_t id[4096] = {0};
            /* Minimal identify controller data */
            id[0] = 0x34; id[1] = 0x12;  /* VID */
            id[4] = 'E'; id[5] = 'D'; id[6] = 'U'; id[7] = 0;  /* Serial */
            id[24] = 'E'; id[25] = 'D'; id[26] = 'U'; id[27] = '-';
            id[28] = 'N'; id[29] = 'V'; id[30] = 'M'; id[31] = 'E';  /* Model */
            /* MDTS (max data transfer size) */
            id[77] = 5;  /* 2^5 * 4KB = 128KB max */
            /* Number of namespaces */
            *(uint32_t *)&id[516] = 1;

            pci_dma_write(&s->parent_obj, cmd->prp1, id, 4096);
        }
        cqe.status = 0;  /* Success */
        break;

    case NVME_CMD_READ:
        lba = cmd->cdw10 | ((uint64_t)cmd->cdw11 << 32);
        nlb = (cmd->cdw12 & 0xFFFF) + 1;
        prp1 = cmd->prp1;

        if (lba + nlb > NUM_BLOCKS) {
            cqe.status = 0x8002;  /* Invalid LBA */
            break;
        }

        res = pci_dma_write(&s->parent_obj, prp1,
                           s->storage + lba * BLOCK_SIZE,
                           nlb * BLOCK_SIZE);
        cqe.status = (res == MEMTX_OK) ? 0 : 0x8004;
        break;

    case NVME_CMD_WRITE:
        lba = cmd->cdw10 | ((uint64_t)cmd->cdw11 << 32);
        nlb = (cmd->cdw12 & 0xFFFF) + 1;
        prp1 = cmd->prp1;

        if (lba + nlb > NUM_BLOCKS) {
            cqe.status = 0x8002;  /* Invalid LBA */
            break;
        }

        res = pci_dma_read(&s->parent_obj, prp1,
                          s->storage + lba * BLOCK_SIZE,
                          nlb * BLOCK_SIZE);
        cqe.status = (res == MEMTX_OK) ? 0 : 0x8004;
        break;

    default:
        cqe.status = 0x0001;  /* Invalid opcode */
        break;
    }

    /* Set phase bit */
    cqe.status |= (s->cq_phase << 8);

    /* Write completion to CQ */
    pci_dma_write(&s->parent_obj, s->acq + s->cq_tail * sizeof(NvmeCqe),
                  &cqe, sizeof(cqe));

    /* Advance CQ tail */
    s->cq_tail++;
    if (s->cq_tail >= (s->aqa >> 16) + 1) {
        s->cq_tail = 0;
        s->cq_phase ^= 1;  /* Toggle phase */
    }

    /* Send MSI-X interrupt */
    if (msix_enabled(&s->parent_obj)) {
        msix_notify(&s->parent_obj, 0);
    }
}

static void edu_nvme_process_sq(EduNvmeState *s)
{
    NvmeCmd cmd;
    uint16_t sq_size = (s->aqa & 0xFFF) + 1;

    while (s->sq_head != s->sq_tail) {
        /* Read command from SQ */
        pci_dma_read(&s->parent_obj, s->asq + s->sq_head * sizeof(NvmeCmd),
                     &cmd, sizeof(cmd));

        edu_nvme_process_cmd(s, &cmd);

        /* Advance SQ head */
        s->sq_head++;
        if (s->sq_head >= sq_size) {
            s->sq_head = 0;
        }
    }
}

static uint64_t edu_nvme_read(void *opaque, hwaddr addr, unsigned size)
{
    EduNvmeState *s = opaque;

    switch (addr) {
    case NVME_CAP:
        /* Capabilities: MQES=63, CQR=1, DSTRD=0 */
        return 0x003F0001;
    case NVME_CAP + 4:
        /* CAP high: MPSMIN=0, MPSMAX=0 */
        return 0;
    case NVME_VS:
        return 0x00010400;  /* Version 1.4 */
    case NVME_CC:
        return s->cc;
    case NVME_CSTS:
        return s->csts;
    case NVME_AQA:
        return s->aqa;
    case NVME_ASQ:
        return s->asq;
    case NVME_ASQ + 4:
        return s->asq >> 32;
    case NVME_ACQ:
        return s->acq;
    case NVME_ACQ + 4:
        return s->acq >> 32;
    default:
        return 0;
    }
}

static void edu_nvme_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    EduNvmeState *s = opaque;

    switch (addr) {
    case NVME_CC:
        s->cc = val;
        if (val & NVME_CC_EN) {
            s->csts |= NVME_CSTS_RDY;
            s->sq_head = s->sq_tail = 0;
            s->cq_head = s->cq_tail = 0;
            s->cq_phase = 1;
        } else {
            s->csts &= ~NVME_CSTS_RDY;
        }
        break;
    case NVME_AQA:
        s->aqa = val;
        break;
    case NVME_ASQ:
        s->asq = (s->asq & 0xFFFFFFFF00000000ULL) | val;
        break;
    case NVME_ASQ + 4:
        s->asq = (s->asq & 0xFFFFFFFF) | ((uint64_t)val << 32);
        break;
    case NVME_ACQ:
        s->acq = (s->acq & 0xFFFFFFFF00000000ULL) | val;
        break;
    case NVME_ACQ + 4:
        s->acq = (s->acq & 0xFFFFFFFF) | ((uint64_t)val << 32);
        break;
    case NVME_SQ0TDBL:
        /* Submission Queue Tail Doorbell */
        s->sq_tail = val;
        edu_nvme_process_sq(s);
        break;
    case NVME_CQ0HDBL:
        /* Completion Queue Head Doorbell */
        s->cq_head = val;
        break;
    }
}

static const MemoryRegionOps edu_nvme_ops = {
    .read = edu_nvme_read,
    .write = edu_nvme_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void edu_nvme_realize(PCIDevice *pdev, Error **errp)
{
    EduNvmeState *s = EDU_NVME(pdev);

    /* Allocate virtual SSD storage */
    s->storage = g_malloc0(STORAGE_SIZE);

    /* Setup BAR0 for registers (8KB for doorbells) */
    memory_region_init_io(&s->mmio, OBJECT(s), &edu_nvme_ops, s,
                          "edu-nvme-mmio", 8192);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* Setup MSI-X with 1 vector in BAR2 */
    if (msix_init_exclusive_bar(pdev, 1, 2, errp)) {
        return;
    }
    msix_vector_use(pdev, 0);

    s->cq_phase = 1;
}

static void edu_nvme_exit(PCIDevice *pdev)
{
    EduNvmeState *s = EDU_NVME(pdev);
    g_free(s->storage);
    msix_uninit_exclusive_bar(pdev);
}

static void edu_nvme_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = edu_nvme_realize;
    k->exit = edu_nvme_exit;
    k->vendor_id = 0x1234;
    k->device_id = 0xED03;
    k->revision = 1;
    k->class_id = PCI_CLASS_STORAGE_EXPRESS;
}

static const TypeInfo edu_nvme_info = {
    .name = TYPE_EDU_NVME,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EduNvmeState),
    .class_init = edu_nvme_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void edu_nvme_register(void)
{
    type_register_static(&edu_nvme_info);
}

type_init(edu_nvme_register)
