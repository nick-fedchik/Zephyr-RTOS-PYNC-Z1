// SPDX-License-Identifier: GPL-2.0
/*
 * Zynq-7000 Remote Processor driver (ported from Xilinx/linux-xlnx to Linux 7.3)
 *
 * Copyright (C) 2012 Michal Simek <monstr@monstr.eu>
 * Copyright (C) 2012 PetaLogix
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/remoteproc.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <../../arch/arm/mach-zynq/common.h>

#include "remoteproc_internal.h"

#define MAX_NUM_VRINGS   2
#define REMOTE_SGI       0
#define HOST_SGI         1
#define GIC_DIST_SOFTINT 0xf00

static int vring_sgis[MAX_NUM_VRINGS] = { 14, 15 };
static int n_vring_sgis = MAX_NUM_VRINGS;

struct ipi_info {
	u32 irq;
	u32 notifyid;
	bool pending;
};

struct zynq_rproc_pdata {
	int irqs[MAX_NUM_VRINGS];
	int virqs[MAX_NUM_VRINGS];
	void __iomem *gic_dist_base;
	struct zynq_rproc_pdata * __percpu *pcpu_dev;
	struct rproc *rproc;
	struct ipi_info ipis[MAX_NUM_VRINGS];
	struct work_struct workqueue;
};

static bool autoboot __read_mostly;

static void zynq_send_sgi(struct zynq_rproc_pdata *local,
			  unsigned int cpu_id, unsigned int sgi)
{
	/* Ensure shared vring writes are visible to CPU1 before raising SGI */
	wmb();
	writel_relaxed(((1U << cpu_id) << 16) | (sgi & 0xfU),
		       local->gic_dist_base + GIC_DIST_SOFTINT);
}

static irqreturn_t zynq_remoteproc_interrupt(int irq, void *dev_id)
{
	struct zynq_rproc_pdata *local = *(struct zynq_rproc_pdata **)dev_id;

	if (local)
		schedule_work(&local->workqueue);
	return IRQ_HANDLED;
}

static void handle_event(struct work_struct *work)
{
	struct zynq_rproc_pdata *local =
		container_of(work, struct zynq_rproc_pdata, workqueue);

	rproc_vq_interrupt(local->rproc, local->ipis[0].notifyid);
}

static void kick_pending_ipi(struct rproc *rproc)
{
	struct zynq_rproc_pdata *local = rproc->priv;
	int i;

	for (i = 0; i < MAX_NUM_VRINGS; i++) {
		if (local->ipis[i].pending) {
			zynq_send_sgi(local, 1, local->irqs[HOST_SGI]);
			local->ipis[i].pending = false;
		}
	}
}

static int zynq_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	int ret;

	ret = remove_cpu(1);
	if (ret && (ret != -EBUSY)) {
		dev_err(dev, "Can't release cpu1: %d\n", ret);
		return ret;
	}

	ret = zynq_cpun_start(rproc->bootaddr, 1);
	kick_pending_ipi(rproc);

	return ret;
}

static void zynq_rproc_kick(struct rproc *rproc, int vqid)
{
	struct zynq_rproc_pdata *local = rproc->priv;
	struct rproc_vdev *rvdev, *rvtmp;
	int i;

	list_for_each_entry_safe(rvdev, rvtmp, &rproc->rvdevs, node) {
		for (i = 0; i < MAX_NUM_VRINGS; i++) {
			struct rproc_vring *rvring = &rvdev->vring[i];

			if (rvring->notifyid == vqid) {
				local->ipis[i].notifyid = vqid;
				if (rproc->state == RPROC_RUNNING)
					zynq_send_sgi(local, 1, local->irqs[REMOTE_SGI]);
				else
					local->ipis[i].pending = true;
			}
		}
	}
}

static int zynq_rproc_stop(struct rproc *rproc)
{
	int ret;
	struct device *dev = rproc->dev.parent;

	ret = add_cpu(1);
	if (ret)
		dev_err(dev, "Can't power on cpu1: %d\n", ret);

	return 0;
}

static int zynq_rproc_mem_alloc(struct rproc *rproc,
				struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	va = ioremap_wc(mem->dma, mem->len);
	if (!va) {
		dev_err(dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	mem->va = va;
	return 0;
}

static int zynq_rproc_mem_release(struct rproc *rproc,
				  struct rproc_mem_entry *mem)
{
	iounmap(mem->va);
	return 0;
}

static int zynq_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int num_mems, i, ret;
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct rproc_mem_entry *mem;

	num_mems = of_count_phandle_with_args(np, "memory-region", NULL);
	if (num_mems <= 0)
		return 0;

	for (i = 0; i < num_mems; i++) {
		struct device_node *node;
		struct reserved_mem *rmem;

		node = of_parse_phandle(np, "memory-region", i);
		rmem = of_reserved_mem_lookup(node);
		if (!rmem) {
			of_node_put(node);
			dev_err(dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}
		if (strstr(node->name, "vdev") && strstr(node->name, "buffer")) {
			mem = rproc_of_resm_mem_entry_init(dev, i, rmem->size,
							   rmem->base,
							   "vdev0buffer");
		} else {
			mem = rproc_mem_entry_init(dev, NULL,
						   (dma_addr_t)rmem->base,
						   rmem->size, rmem->base,
						   zynq_rproc_mem_alloc,
						   zynq_rproc_mem_release,
						   node->name);
		}
		of_node_put(node);
		if (!mem)
			return -ENOMEM;
		rproc_add_carveout(rproc, mem);
	}

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret == -EINVAL)
		ret = 0;
	return ret;
}

static const struct rproc_ops zynq_rproc_ops = {
	.start			= zynq_rproc_start,
	.stop			= zynq_rproc_stop,
	.load			= rproc_elf_load_segments,
	.parse_fw		= zynq_parse_fw,
	.find_loaded_rsc_table	= rproc_elf_find_loaded_rsc_table,
	.get_boot_addr		= rproc_elf_get_boot_addr,
	.kick			= zynq_rproc_kick,
};

static int zynq_remoteproc_probe(struct platform_device *pdev)
{
	int ret, i, cpu;
	struct rproc *rproc;
	struct zynq_rproc_pdata *local;
	struct irq_domain *domain;
	struct irq_fwspec sgi_fwspec;
	struct device_node *interrupt_parent, *node = pdev->dev.of_node;

	rproc = rproc_alloc(&pdev->dev, dev_name(&pdev->dev),
			    &zynq_rproc_ops, NULL,
			    sizeof(struct zynq_rproc_pdata));
	if (!rproc)
		return -ENOMEM;

	local = rproc->priv;
	local->rproc = rproc;
	INIT_WORK(&local->workqueue, handle_event);
	platform_set_drvdata(pdev, rproc);

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free;

	if (n_vring_sgis != MAX_NUM_VRINGS) {
		ret = -EINVAL;
		goto err_free;
	}

	interrupt_parent = of_irq_find_parent(node);
	if (!interrupt_parent) {
		ret = -EINVAL;
		goto err_free;
	}

	local->gic_dist_base = of_iomap(interrupt_parent, 0);
	domain = irq_find_host(interrupt_parent);
	of_node_put(interrupt_parent);
	if (!local->gic_dist_base) {
		ret = -ENOMEM;
		goto err_free;
	}
	if (!domain) {
		ret = -EPROBE_DEFER;
		goto err_iounmap;
	}

	local->pcpu_dev = alloc_percpu(struct zynq_rproc_pdata *);
	if (!local->pcpu_dev) {
		ret = -ENOMEM;
		goto err_iounmap;
	}
	for_each_possible_cpu(cpu)
		*per_cpu_ptr(local->pcpu_dev, cpu) = local;

	sgi_fwspec.fwnode = domain->fwnode;
	sgi_fwspec.param_count = 1;

	for (i = 0; i < MAX_NUM_VRINGS; i++) {
		sgi_fwspec.param[0] = vring_sgis[i];
		local->virqs[i] = irq_create_fwspec_mapping(&sgi_fwspec);
		if (!local->virqs[i]) {
			dev_err(&pdev->dev, "failed to map SGI %d\n", vring_sgis[i]);
			ret = -EINVAL;
			goto err_irqs;
		}
		ret = request_percpu_irq(local->virqs[i],
					 zynq_remoteproc_interrupt,
					 "zynq_rproc_sgi", local->pcpu_dev);
		if (ret) {
			dev_err(&pdev->dev, "request_percpu_irq SGI %d failed: %d\n",
				vring_sgis[i], ret);
			goto err_irqs;
		}
		enable_percpu_irq(local->virqs[i], 0);
		local->irqs[i] = vring_sgis[i];
	}

	rproc->auto_boot = autoboot;

	ret = rproc_add(local->rproc);
	if (ret)
		goto err_irqs;

	return 0;

err_irqs:
	while (--i >= 0) {
		disable_percpu_irq(local->virqs[i]);
		free_percpu_irq(local->virqs[i], local->pcpu_dev);
	}
	free_percpu(local->pcpu_dev);
err_iounmap:
	iounmap(local->gic_dist_base);
err_free:
	rproc_free(rproc);
	return ret;
}

static void zynq_remoteproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct zynq_rproc_pdata *local = rproc->priv;
	int i;

	rproc_del(rproc);
	for (i = 0; i < MAX_NUM_VRINGS; i++) {
		disable_percpu_irq(local->virqs[i]);
		free_percpu_irq(local->virqs[i], local->pcpu_dev);
	}
	free_percpu(local->pcpu_dev);
	iounmap(local->gic_dist_base);
	of_reserved_mem_device_release(&pdev->dev);
	rproc_free(rproc);
}

static const struct of_device_id zynq_remoteproc_match[] = {
	{ .compatible = "xlnx,zynq_remoteproc", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, zynq_remoteproc_match);

static struct platform_driver zynq_remoteproc_driver = {
	.probe = zynq_remoteproc_probe,
	.remove = zynq_remoteproc_remove,
	.driver = {
		.name = "zynq_remoteproc",
		.of_match_table = zynq_remoteproc_match,
	},
};
module_platform_driver(zynq_remoteproc_driver);

module_param_named(autoboot, autoboot, bool, 0444);
module_param_array(vring_sgis, int, &n_vring_sgis, 0);

MODULE_AUTHOR("Michal Simek <monstr@monstr.eu>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zynq-7000 remote processor control driver");
