/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Copyright 2017 NXP
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/platform_device.h>
#include <linux/of.h>

struct imx8_soc_data {
	char *name;
	u32 (*soc_revision)(void);
};

u32 imx8qm_soc_revision(void);
u32 imx8qxp_soc_revision(void);

struct imx8_soc_data imx8qm_soc_data = {
	.name = "i.MX8QM",
	.soc_revision = imx8qm_soc_revision,
};

struct imx8_soc_data imx8qxp_soc_data = {
	.name = "i.MX8QXP",
	.soc_revision = imx8qxp_soc_revision,
};

static const struct of_device_id imx8_soc_match[] = {
	{ .compatible = "fsl,imx8qm", .data = &imx8qm_soc_data, },
	{ .compatible = "fsl,imx8qxp", .data = &imx8qxp_soc_data, },
	{ }
};

u32 imx8qm_soc_revision(void)
{
	/*FIX ME later */

	return 0x10;
}

u32 imx8qxp_soc_revision(void)
{
	/*FIX ME later */

	return 0x10;
}

static int __init imx8_soc_init(void)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *root;
	const struct of_device_id *id;
	u32 imx_soc_revision = 0;
	const struct imx8_soc_data *data;
	int ret;

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENODEV;

	soc_dev_attr->family = "Freescale i.MX";

	root = of_find_node_by_path("/");
	ret = of_property_read_string(root, "model", &soc_dev_attr->machine);
	if (ret)
		goto free_soc;

	id = of_match_node(imx8_soc_match, root);
	if (!id)
		goto free_soc;

	of_node_put(root);

	data = id->data;
	if (data) {
		soc_dev_attr->soc_id = data->name;
		if (data->soc_revision)
			imx_soc_revision = data->soc_revision();
	}

	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%d.%d",
					   (imx_soc_revision >> 4) & 0xf,
					   imx_soc_revision & 0xf);
	if (!soc_dev_attr->revision)
		goto free_soc;

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev))
		goto free_rev;

	return 0;

free_rev:
	kfree(soc_dev_attr->revision);
free_soc:
	kfree(soc_dev_attr);
	return -ENODEV;
}
device_initcall(imx8_soc_init);
