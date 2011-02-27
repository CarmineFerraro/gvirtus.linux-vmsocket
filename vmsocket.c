/*
 * linux-vmsocket -- Guest driver for the VMSocket PCI Device.
 *
 * Copyright (C) 2009-2010  The University of Napoli Parthenope at Naples.
 *
 * This file is part of linux-vmsocket.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Written by: Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>,
 *             Department of Applied Science
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/seq_file.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <asm/system.h>
#include <asm/uaccess.h>

MODULE_DESCRIPTION("Guest driver for the VMSocket PCI Device.");
MODULE_VERSION("0.2-pre1");
MODULE_AUTHOR("Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>");
MODULE_LICENSE("GPL");

#define VMSOCKET_ERR(fmt, args...) printk( KERN_ERR "vmsocket: " fmt "\n", ## args)
#define VMSOCKET_INFO(fmt, args...) printk( KERN_INFO "vmsocket: " fmt "\n", ## args)

#ifndef VMSOCKET_MAJOR
#define VMSOCKET_MAJOR 0
#endif

/* Registers */
/* Read Only */
#define VMSOCKET_CONNECT_L_REG(dev)    ((dev)->regs)
#define VMSOCKET_READ_L_REG(dev)       ((dev)->regs + 0x20)
#define VMSOCKET_WRITE_L_REG(dev)      ((dev)->regs + 0x40)
#define VMSOCKET_FSYNC_L_REG(dev)      ((dev)->regs + 0x60)
#define VMSOCKET_CLOSE_L_REG(dev)      ((dev)->regs + 0x80)

typedef struct VMSocketCtrl {
    char path[1024];
    uint32_t bytes_to_read;
    uint32_t bytes_to_write;
} VMSocketCtrl;

struct vmsocket_dev {
	struct pci_dev *pdev;

	void __iomem *regs;
	uint32_t regaddr;
	uint32_t reg_size;

	void *in_buffer;
	uint32_t in_size;
	uint32_t in_addr;

	void *out_buffer;
	uint32_t out_size;
	uint32_t out_addr;

	VMSocketCtrl *ctrl;
	uint32_t ctrl_size;
	uint32_t ctrl_addr;

	struct cdev cdev;

	atomic_t available;
};

static struct vmsocket_dev vmsocket_dev[128];
static struct class *fc = NULL;
int vmsocket_major = VMSOCKET_MAJOR;
int vmsocket_minor = 0;

static int vmsocket_open(struct inode *inode, struct file *filp)
{
	struct vmsocket_dev *dev = &vmsocket_dev[MINOR(inode->i_rdev)];

	if (!atomic_dec_and_test(&dev->available)) {
		atomic_inc(&dev->available);
		return -EBUSY;
	}

	filp->private_data = dev;
	return 0;
}

static int vmsocket_release(struct inode *inode, struct file *filp)
{
	struct vmsocket_dev *dev = filp->private_data;
	int status = 0;

	if ((status = readl(VMSOCKET_CLOSE_L_REG(dev))) != 0)
		VMSOCKET_ERR("can't close connection.");
	atomic_inc(&dev->available);
	return status;
}

int vmsocket_ioctl(struct inode *inode, struct file *filp,
		unsigned int ioctl_num, unsigned long ioctl_param)
{
	struct vmsocket_dev *dev = filp->private_data;
	size_t size = 0;
	char *path = (char *)ioctl_param;
	for (size = 0; size < sizeof(dev->ctrl->path) && path[size] != 0; size++)
		dev->ctrl->path[size] = path[size];
	dev->ctrl->path[size] = 0;

	if (readl(VMSOCKET_CONNECT_L_REG(dev)) != 0)
		return -1;

	VMSOCKET_INFO("connected to %s.", dev->ctrl->path);

	return 0;
}

ssize_t vmsocket_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct vmsocket_dev *dev = filp->private_data;
	int readed;
	dev->ctrl->bytes_to_read = min(count, (size_t) dev->in_size);
	//set_current_state(TASK_INTERRUPTIBLE);
	while((readed = readl(VMSOCKET_READ_L_REG(dev))) > (int) dev->ctrl->bytes_to_read) {
		VMSOCKET_ERR("readed: %d\n", readed);
		//msleep(200);
		//schedule();
		//set_current_state(TASK_INTERRUPTIBLE);
	}
	//__set_current_state(TASK_RUNNING);
	if(copy_to_user(buf, dev->in_buffer, readed))
		return -EFAULT;
	return readed;
}

ssize_t vmsocket_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct vmsocket_dev *dev = filp->private_data;
	dev->ctrl->bytes_to_write = min(count, (size_t) dev->out_size);
	if(copy_from_user(dev->out_buffer, buf, dev->ctrl->bytes_to_write))
		return -EFAULT;
	return readl(VMSOCKET_WRITE_L_REG(dev));
}

int vmsocket_fsync(struct file *filp, struct dentry *dentryp, int datasync)
{
	struct vmsocket_dev *dev = filp->private_data;
	return readl(VMSOCKET_FSYNC_L_REG(dev));
}

static const struct file_operations vmsocket_fops = {
	.owner = THIS_MODULE,
	.open = vmsocket_open,
	.release = vmsocket_release,
	.read = vmsocket_read,
	.write = vmsocket_write,
	.fsync = vmsocket_fsync,
	.ioctl = vmsocket_ioctl
};

static struct pci_device_id vmsocket_id_table[] = {
	{0x1af4, 0x6662, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0},
};

MODULE_DEVICE_TABLE(pci, vmsocket_id_table);

static int vmsocket_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int result;

	result = pci_enable_device(pdev);

	if (result) {
		VMSOCKET_ERR("cannot probe device %s: error %d.",
			  pci_name(pdev), result);
		return result;
	}

	vmsocket_dev[vmsocket_minor].pdev = pdev;

	result = pci_request_regions(pdev, "vmsocket");
	if (result < 0) {
		VMSOCKET_ERR("cannot request regions.");
		goto pci_disable;
	}

	/* Registers */
	vmsocket_dev[vmsocket_minor].regaddr = pci_resource_start(pdev, 0);
	vmsocket_dev[vmsocket_minor].reg_size = pci_resource_len(pdev, 0);
	vmsocket_dev[vmsocket_minor].regs = pci_iomap(pdev, 0, 0x100);
	if (!vmsocket_dev[vmsocket_minor].regs) {
		VMSOCKET_ERR("cannot ioremap registers.");
		goto reg_release;
	}

	/* I/O Buffers */
	vmsocket_dev[vmsocket_minor].in_addr = pci_resource_start(pdev, 1);
	vmsocket_dev[vmsocket_minor].in_buffer = pci_iomap(pdev, 1, 0);
	vmsocket_dev[vmsocket_minor].in_size = pci_resource_len(pdev, 1);
	if (!vmsocket_dev[vmsocket_minor].in_buffer) {
		VMSOCKET_ERR("cannot ioremap input buffer.");
		goto in_release;
	}

	vmsocket_dev[vmsocket_minor].out_addr = pci_resource_start(pdev, 2);
	vmsocket_dev[vmsocket_minor].out_buffer = pci_iomap(pdev, 2, 0);
	vmsocket_dev[vmsocket_minor].out_size = pci_resource_len(pdev, 2);
	if (!vmsocket_dev[vmsocket_minor].out_buffer) {
		VMSOCKET_ERR("cannot ioremap output buffer.");
		goto out_release;
	}

	vmsocket_dev[vmsocket_minor].ctrl_addr = pci_resource_start(pdev, 3);
	vmsocket_dev[vmsocket_minor].ctrl = pci_iomap(pdev, 3, 0);
	vmsocket_dev[vmsocket_minor].ctrl_size = pci_resource_len(pdev, 3);
	if (!vmsocket_dev[vmsocket_minor].ctrl) {
		VMSOCKET_ERR("cannot ioremap ctrl.");
		goto ctrl_release;
	}

	atomic_set(&vmsocket_dev[vmsocket_minor].available, 1);

	cdev_init(&vmsocket_dev[vmsocket_minor].cdev, &vmsocket_fops);
	vmsocket_dev[vmsocket_minor].cdev.owner = THIS_MODULE;
	vmsocket_dev[vmsocket_minor].cdev.ops = &vmsocket_fops;
	result = cdev_add(&vmsocket_dev[vmsocket_minor].cdev, MKDEV(vmsocket_major, 
							vmsocket_minor), 1);
	if (result)
		VMSOCKET_ERR("error %d adding vmsocket%d", result, vmsocket_minor);

	VMSOCKET_INFO("registered device, major: %d minor: %d.",
		   vmsocket_major, vmsocket_minor);

	/* create sysfs entry */
	if (fc == NULL)
		fc = class_create(THIS_MODULE, "vmsocket");
	device_create(fc, NULL, vmsocket_dev[vmsocket_minor].cdev.dev, NULL, "%s%d", 
		      "vmsocket", vmsocket_minor);
	vmsocket_minor++;

	return 0;

ctrl_release:
	pci_iounmap(pdev, vmsocket_dev[vmsocket_minor].out_buffer);
out_release:
	pci_iounmap(pdev, vmsocket_dev[vmsocket_minor].in_buffer);
in_release:
	pci_iounmap(pdev, vmsocket_dev[vmsocket_minor].regs);
reg_release:
	pci_release_regions(pdev);
pci_disable:
	pci_disable_device(pdev);
	return -EBUSY;
}

static void vmsocket_remove(struct pci_dev *pdev)
{
	int i = 0;
	VMSOCKET_INFO("unregistered device.");
	for(i = 0; i < vmsocket_minor; i++) {
		if(vmsocket_dev[i].pdev == pdev)
			break;
	}

	device_destroy(fc, vmsocket_dev[i].cdev.dev);
	pci_iounmap(pdev, vmsocket_dev[i].regs);
	pci_iounmap(pdev, vmsocket_dev[i].in_buffer);
	pci_iounmap(pdev, vmsocket_dev[i].out_buffer);
	pci_iounmap(pdev, vmsocket_dev[i].ctrl);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	if (fc != NULL) {
		class_destroy(fc);
		fc = NULL;
	}

}

static struct pci_driver vmsocket_pci_driver = {
	.name = "vmsocket",
	.id_table = vmsocket_id_table,
	.probe = vmsocket_probe,
	.remove = vmsocket_remove,
};

static int __init vmsocket_init_module(void)
{
	int result;
	dev_t dev = 0;

	if (vmsocket_major) {
		dev = MKDEV(vmsocket_major, vmsocket_minor);
		result = register_chrdev_region(dev, 1, "vmsocket");
	} else {
		result = alloc_chrdev_region(&dev, vmsocket_minor, 1, "vmsocket");
		vmsocket_major = MAJOR(dev);
	}

	if (result < 0) {
		VMSOCKET_ERR("can't get major %d.", vmsocket_major);
		return result;
	}

	if ((result = pci_register_driver(&vmsocket_pci_driver)) != 0) {
		VMSOCKET_ERR("can't register PCI driver.");
		return result;
	}

	return 0;
}

module_init(vmsocket_init_module);

static void __exit vmsocket_exit(void)
{
	int i;
	for(i = 0; i < vmsocket_minor; i++)
		cdev_del(&vmsocket_dev[i].cdev);
	pci_unregister_driver(&vmsocket_pci_driver);
	unregister_chrdev_region(MKDEV(vmsocket_major, vmsocket_minor), 1);
}

module_exit(vmsocket_exit);
