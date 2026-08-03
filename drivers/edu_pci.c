/*
 * edu_pci - Linux kernel driver for QEMU edu-pci device
 * 
 * Step 2: Map BAR0 and read registers
 */
#include <linux/module.h>
#include <linux/pci.h>

#define EDU_VENDOR_ID  0x1234
#define EDU_DEVICE_ID  0xED01

/* Register offsets */
#define REG_ID       0x00
#define REG_SCRATCH  0x04

/* Per-device data */
struct edu_device {
    void __iomem *bar0;  /* Mapped BAR0 */
};

/* Called when device is found */
static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct edu_device *edu;
    u32 device_id, scratch;
    int ret;

    pr_info("edu_pci: probe device %s\n", pci_name(pdev));

    /* Allocate driver data */
    edu = kzalloc(sizeof(*edu), GFP_KERNEL);
    if (!edu)
        return -ENOMEM;

    /* Enable the PCI device */
    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free;

    /* Request BAR0 region */
    ret = pci_request_region(pdev, 0, "edu_pci");
    if (ret)
        goto err_disable;

    /* Map BAR0 */
    edu->bar0 = pci_iomap(pdev, 0, 0);
    if (!edu->bar0) {
        ret = -ENOMEM;
        goto err_release;
    }

    /* Store driver data */
    pci_set_drvdata(pdev, edu);

    /* Read device registers */
    device_id = ioread32(edu->bar0 + REG_ID);
    pr_info("edu_pci: Device ID = 0x%08X\n", device_id);

    /* Test scratch register */
    iowrite32(0xDEADBEEF, edu->bar0 + REG_SCRATCH);
    scratch = ioread32(edu->bar0 + REG_SCRATCH);
    pr_info("edu_pci: Scratch = 0x%08X\n", scratch);

    pr_info("edu_pci: device ready!\n");
    return 0;

err_release:
    pci_release_region(pdev, 0);
err_disable:
    pci_disable_device(pdev);
err_free:
    kfree(edu);
    return ret;
}

/* Called when device is removed */
static void edu_remove(struct pci_dev *pdev)
{
    struct edu_device *edu = pci_get_drvdata(pdev);

    pr_info("edu_pci: remove device %s\n", pci_name(pdev));

    pci_iounmap(pdev, edu->bar0);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);
    kfree(edu);
}

/* Device ID table - which devices this driver supports */
static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0 }  /* terminator */
};
MODULE_DEVICE_TABLE(pci, edu_ids);

/* PCI driver structure */
static struct pci_driver edu_driver = {
    .name     = "edu_pci",
    .id_table = edu_ids,
    .probe    = edu_probe,
    .remove   = edu_remove,
};

/* Register driver on module load */
module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Driver for QEMU edu-pci educational device");
