/*
 * Copyright (C) 2013 Xilinx
 *
 * Based on "Generic on-chip SRAM allocation driver"
 *
 * Copyright (C) 2012 Philipp Zabel, Pengutronix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/genalloc.h>

#include "common.h"

#define ZYNQ_OCM_HIGHADDR	0xfffc0000
#define ZYNQ_OCM_LOWADDR	0x0
#define ZYNQ_OCM_BLOCK_SIZE	0x10000
#define ZYNQ_OCM_BLOCKS		4
#define ZYNQ_OCM_GRANULARITY	32

#define ZYNQ_OCM_PARITY_CTRL	0x0
#define ZYNQ_OCM_PARITY_ENABLE	0x1e

#define ZYNQ_OCM_PARITY_ERRADDRESS	0x4

#define ZYNQ_OCM_IRQ_STS		0x8
#define ZYNQ_OCM_IRQ_STS_ERR_MASK	0x7

struct zynq_ocm_dev {
	void __iomem *base;
	int irq;
	struct gen_pool *pool;
	struct resource res[ZYNQ_OCM_BLOCKS];
};

/**
 * zynq_ocm_irq_handler - Interrupt service routine of the OCM controller
 * @irq:	IRQ number
 * @data:	Pointer to the zynq_ocm_dev structure
 *
 * Return:	IRQ_HANDLED when handled; IRQ_NONE otherwise.
 */
static irqreturn_t zynq_ocm_irq_handler(int irq, void *data)
{
	u32 sts;
	u32 err_addr;
	struct zynq_ocm_dev *zynq_ocm = data;

	/* check status */
	sts = readl(zynq_ocm->base + ZYNQ_OCM_IRQ_STS);
	if (sts & ZYNQ_OCM_IRQ_STS_ERR_MASK) {
		/* check error address */
		err_addr = readl(zynq_ocm->base + ZYNQ_OCM_PARITY_ERRADDRESS);
		pr_err("%s: OCM err intr generated at 0x%04x (stat: 0x%08x).",
		       __func__, err_addr, sts & ZYNQ_OCM_IRQ_STS_ERR_MASK);
		return IRQ_HANDLED;
	}
	pr_warn("%s: Interrupt generated by OCM, but no error is found.",
		__func__);

	return IRQ_NONE;
}

/**
 * zynq_ocm_probe - Probe method for the OCM driver
 * @pdev:	Pointer to the platform_device structure
 *
 * This function initializes the driver data structures and the hardware.
 *
 * Return:	0 on success and error value on failure
 */
static int zynq_ocm_probe(struct platform_device *pdev)
{
	int ret;
	struct zynq_ocm_dev *zynq_ocm;
	u32 i, ocm_config, curr;
	struct resource *res;

	ocm_config = zynq_slcr_get_ocm_config();

	zynq_ocm = devm_kzalloc(&pdev->dev, sizeof(*zynq_ocm), GFP_KERNEL);
	if (!zynq_ocm)
		return -ENOMEM;

	zynq_ocm->pool = devm_gen_pool_create(&pdev->dev,
					      ilog2(ZYNQ_OCM_GRANULARITY), -1);
	if (!zynq_ocm->pool)
		return -ENOMEM;

	curr = 0; /* For storing current struct resource for OCM */
	for (i = 0; i < ZYNQ_OCM_BLOCKS; i++) {
		u32 base, start, end;

		/* Setup base address for 64kB OCM block */
		if (ocm_config & BIT(i))
			base = ZYNQ_OCM_HIGHADDR;
		else
			base = ZYNQ_OCM_LOWADDR;

		/* Calculate start and end block addresses */
		start = i * ZYNQ_OCM_BLOCK_SIZE + base;
		end = start + (ZYNQ_OCM_BLOCK_SIZE - 1);

		/* Concatenate OCM blocks together to get bigger pool */
		if (i > 0 && start == (zynq_ocm->res[curr - 1].end + 1)) {
			zynq_ocm->res[curr - 1].end = end;
		} else {
#ifdef CONFIG_SMP
			/*
			 * OCM block if placed at 0x0 has special meaning
			 * for SMP because jump trampoline is added there.
			 * Ensure that this address won't be allocated.
			 */
			if (!base) {
				u32 trampoline_code_size =
					&zynq_secondary_trampoline_end -
					&zynq_secondary_trampoline;
				dev_dbg(&pdev->dev,
					"Allocate reset vector table %dB\n",
					trampoline_code_size);
				/* postpone start offset */
				start += trampoline_code_size;
			}
#endif
			/* First resource is always initialized */
			zynq_ocm->res[curr].start = start;
			zynq_ocm->res[curr].end = end;
			zynq_ocm->res[curr].flags = IORESOURCE_MEM;
			curr++; /* Increment curr value */
		}
		dev_dbg(&pdev->dev, "OCM block %d, start %x, end %x\n",
			i, start, end);
	}

	/*
	 * Separate pool allocation from OCM block detection to ensure
	 * the biggest possible pool.
	 */
	for (i = 0; i < ZYNQ_OCM_BLOCKS; i++) {
		unsigned long size;
		void __iomem *virt_base;

		/* Skip all zero size resources */
		if (zynq_ocm->res[i].end == 0)
			break;
		dev_dbg(&pdev->dev, "OCM resources %d, start %x, end %x\n",
			i, zynq_ocm->res[i].start, zynq_ocm->res[i].end);
		size = resource_size(&zynq_ocm->res[i]);
		virt_base = devm_ioremap_resource(&pdev->dev,
						  &zynq_ocm->res[i]);
		if (IS_ERR(virt_base))
			return PTR_ERR(virt_base);

		ret = gen_pool_add_virt(zynq_ocm->pool,
					(unsigned long)virt_base,
					zynq_ocm->res[i].start, size, -1);
		if (ret < 0) {
			dev_err(&pdev->dev, "Gen pool failed\n");
			return ret;
		}
		dev_info(&pdev->dev, "ZYNQ OCM pool: %ld KiB @ 0x%p\n",
			 size / 1024, virt_base);
	}

	/* Get OCM config space */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	zynq_ocm->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(zynq_ocm->base))
		return PTR_ERR(zynq_ocm->base);

	/* Allocate OCM parity IRQ */
	zynq_ocm->irq = platform_get_irq(pdev, 0);
	if (zynq_ocm->irq < 0) {
		dev_err(&pdev->dev, "irq resource not found\n");
		return zynq_ocm->irq;
	}
	ret = devm_request_irq(&pdev->dev, zynq_ocm->irq, zynq_ocm_irq_handler,
			       0, pdev->name, zynq_ocm);
	if (ret != 0) {
		dev_err(&pdev->dev, "request_irq failed\n");
		return ret;
	}
	/* Enable parity errors */
	writel(ZYNQ_OCM_PARITY_ENABLE, zynq_ocm->base + ZYNQ_OCM_PARITY_CTRL);

	platform_set_drvdata(pdev, zynq_ocm);

	return 0;
}

/**
 * zynq_ocm_remove - Remove method for the OCM driver
 * @pdev:	Pointer to the platform_device structure
 *
 * This function is called if a device is physically removed from the system or
 * if the driver module is being unloaded. It frees all resources allocated to
 * the device.
 *
 * Return:	0 on success and error value on failure
 */
static int zynq_ocm_remove(struct platform_device *pdev)
{
	struct zynq_ocm_dev *zynq_ocm = platform_get_drvdata(pdev);

	if (gen_pool_avail(zynq_ocm->pool) < gen_pool_size(zynq_ocm->pool))
		dev_dbg(&pdev->dev, "removed while SRAM allocated\n");

	return 0;
}

static struct of_device_id zynq_ocm_dt_ids[] = {
	{ .compatible = "xlnx,zynq-ocmc-1.0" },
	{ /* end of table */ }
};

static struct platform_driver zynq_ocm_driver = {
	.driver = {
		.name = "zynq-ocm",
		.of_match_table = zynq_ocm_dt_ids,
	},
	.probe = zynq_ocm_probe,
	.remove = zynq_ocm_remove,
};

static int __init zynq_ocm_init(void)
{
	
	return platform_driver_register(&zynq_ocm_driver);
}

arch_initcall(zynq_ocm_init);
