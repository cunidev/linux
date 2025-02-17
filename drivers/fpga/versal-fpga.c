// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2021 Xilinx, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <linux/firmware/xlnx-zynqmp.h>

static int versal_fpga_ops_write_init(struct fpga_manager *mgr,
				      struct fpga_image_info *info,
				      const char *buf, size_t size)
{
	return 0;
}

static int versal_fpga_ops_write(struct fpga_manager *mgr,
				 const char *buf, size_t size)
{
	dma_addr_t dma_addr = 0;
	char *kbuf;
	int ret;

	kbuf = dma_alloc_coherent(mgr->dev.parent, size, &dma_addr, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	memcpy(kbuf, buf, size);
	ret = zynqmp_pm_load_pdi(PDI_SRC_DDR, dma_addr);
	dma_free_coherent(mgr->dev.parent, size, kbuf, dma_addr);

	return ret;
}

static int versal_fpga_ops_write_complete(struct fpga_manager *mgr,
					  struct fpga_image_info *info)
{
	return 0;
}

static enum fpga_mgr_states versal_fpga_ops_state(struct fpga_manager *mgr)
{
	return FPGA_MGR_STATE_UNKNOWN;
}

static const struct fpga_manager_ops versal_fpga_ops = {
	.state = versal_fpga_ops_state,
	.write_init = versal_fpga_ops_write_init,
	.write = versal_fpga_ops_write,
	.write_complete = versal_fpga_ops_write_complete,
};

static int versal_fpga_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpga_manager *mgr;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(dev, "no usable DMA configuration\n");
		return ret;
	}

	mgr = devm_fpga_mgr_create(dev, "Xilinx Versal FPGA Manager",
				   &versal_fpga_ops, NULL);
	if (!mgr)
		return -ENOMEM;

	return devm_fpga_mgr_register(dev, mgr);
}

static const struct of_device_id versal_fpga_of_match[] = {
	{ .compatible = "xlnx,versal-fpga", },
	{},
};
MODULE_DEVICE_TABLE(of, versal_fpga_of_match);

static struct platform_driver versal_fpga_driver = {
	.probe = versal_fpga_probe,
	.driver = {
		.name = "versal_fpga_manager",
		.of_match_table = of_match_ptr(versal_fpga_of_match),
	},
};
module_platform_driver(versal_fpga_driver);

MODULE_AUTHOR("Nava kishore Manne <nava.manne@xilinx.com>");
MODULE_AUTHOR("Appana Durga Kedareswara rao <appanad.durga.rao@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Versal FPGA Manager");
MODULE_LICENSE("GPL");
