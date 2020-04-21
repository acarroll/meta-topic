/*
 * dyplo-of.c
 *
 * Dyplo loadable kernel module.
 *
 * (C) Copyright 2013,2014 Topic Embedded Products B.V. (http://www.topic.nl).
 * All rights reserved.
 *
 * This file is part of kernel-module-dyplo.
 * kernel-module-dyplo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * kernel-module-dyplo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with <product name>.  If not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA or see <http://www.gnu.org/licenses/>.
 *
 * You can contact Topic by electronic mail via info@topic.nl or via
 * paper mail at the following address: Postbus 440, 5680 AK Best, The Netherlands.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Topic Embedded Products <www.topic.nl>");

static bool xvip_is_mplane = true;
MODULE_PARM_DESC(is_mplane,
		 "v4l2 device capability to handle multi planar formats");
module_param_named(is_mplane, xvip_is_mplane, bool, 0444);

/**
 * struct xvip_composite_device - Xilinx Video IP device structure
 * @v4l2_dev: V4L2 device
 * @media_dev: media device
 * @dev: (OF) device
 * @notifier: V4L2 asynchronous subdevs notifier
 * @entities: entities in the graph as a list of xvip_graph_entity
 * @num_subdevs: number of subdevs in the pipeline
 * @dmas: list of DMA channels at the pipeline output and input
 * @v4l2_caps: V4L2 capabilities of the whole device (see VIDIOC_QUERYCAP)
 * @lock: This is to ensure all dma path entities acquire same pipeline object
 */
struct xvip_composite_device {
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	struct device *dev;

	struct v4l2_async_notifier notifier;
	struct list_head entities;
	unsigned int num_subdevs;

	struct list_head dmas;
	u32 v4l2_caps;
	struct mutex lock; /* lock to protect xvip pipeline instance */
};


/**
 * struct xvip_graph_entity - Entity in the video graph
 * @list: list entry in a graph entities list
 * @node: the entity's DT node
 * @entity: media entity, from the corresponding V4L2 subdev
 * @asd: subdev asynchronous registration information
 * @subdev: V4L2 subdev
 * @streaming: status of the V4L2 subdev if streaming or not
 */
struct xvip_graph_entity {
	struct list_head list;
	struct device_node *node;
	struct media_entity *entity;

	struct v4l2_async_subdev asd;
	struct v4l2_subdev *subdev;
	bool streaming;
};

/* Prototypes for device functions */
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static struct xvip_composite_device *g_xdev;
static int device_open_count = 0;
static int indication = 0;

/* -----------------------------------------------------------------------------
 * Graph Management
 */

static struct xvip_graph_entity *
xvip_graph_find_entity(struct xvip_composite_device *xdev,
		       const struct device_node *node)
{
	struct xvip_graph_entity *entity;

	list_for_each_entry(entity, &xdev->entities, list) {
		if (entity->node == node)
			return entity;
	}

	return NULL;
}

static int xvip_graph_build_one(struct xvip_composite_device *xdev,
				struct xvip_graph_entity *entity)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *local = entity->entity;
	struct media_entity *remote;
	struct media_pad *local_pad;
	struct media_pad *remote_pad;
	struct xvip_graph_entity *ent;
	struct v4l2_fwnode_link link;
	struct device_node *ep = NULL;
	int ret = 0;

	//dev_dbg(xdev->dev, "creating links for entity (%s)\n", local->name);
	if(strcmp(local->name,"IMX274") == 0) {
		if(indication == 0)
			local->name = "IMX274_0";
		else
			local->name = "IMX274_1";
		indication++;
		//dev_dbg(xdev->dev, "device is now (%s)\n", local->name);
	}

	while (1) {
		/* Get the next endpoint and parse its link. */
		ep = of_graph_get_next_endpoint(entity->node, ep);
		if (ep == NULL)
			break;

		//dev_dbg(xdev->dev, "processing endpoint %pOF\n", ep);

		ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep), &link);
		if (ret < 0) {
			dev_err(xdev->dev, "failed to parse link for %pOF\n",
				ep);
			continue;
		}

		/* Skip sink ports, they will be processed from the other end of
		 * the link.
		 */
		if (link.local_port >= local->num_pads) {
			dev_err(xdev->dev, "invalid port number %u for %pOF\n",
				link.local_port,
				to_of_node(link.local_node));
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		local_pad = &local->pads[link.local_port];

		if (local_pad->flags & MEDIA_PAD_FL_SINK) {
			//dev_dbg(xdev->dev, "skipping sink port %pOF:%u\n",
			//	to_of_node(link.local_node),
			//	link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Skip DMA engines, they will be processed separately. */
		if (link.remote_node == of_fwnode_handle(xdev->dev->of_node)) {
			//dev_dbg(xdev->dev, "skipping DMA port %pOF:%u\n",
			//	to_of_node(link.local_node),
			//	link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Find the remote entity. */
		ent = xvip_graph_find_entity(xdev,
					     to_of_node(link.remote_node));
		if (ent == NULL) {
			dev_err(xdev->dev, "no entity found for %pOF\n",
				to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		remote = ent->entity;

		if (link.remote_port >= remote->num_pads) {
			dev_err(xdev->dev, "invalid port number %u on %pOF\n",
				link.remote_port, to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		remote_pad = &remote->pads[link.remote_port];

		v4l2_fwnode_put_link(&link);

		/* Create the media link. */
		//dev_dbg(xdev->dev, "creating %s:%u -> %s:%u link\n",
		//	local->name, local_pad->index,
		//	remote->name, remote_pad->index);

		ret = media_create_pad_link(local, local_pad->index,
					       remote, remote_pad->index,
					       link_flags);
		if (ret < 0) {
			dev_err(xdev->dev,
				"failed to create %s:%u -> %s:%u link\n",
				local->name, local_pad->index,
				remote->name, remote_pad->index);
			break;
		}
	}

	return ret;
}


/**
 * xvip_subdev_set_streaming - Find and update streaming status of subdev
 * @xdev: Composite video device
 * @subdev: V4L2 sub-device
 * @enable: enable/disable streaming status
 *
 * Walk the xvip graph entities list and find if subdev is present. Returns
 * streaming status of subdev and update the status as requested
 *
 * Return: streaming status (true or false) if successful or warn_on if subdev
 * is not present and return false
 */
bool xvip_subdev_set_streaming(struct xvip_composite_device *xdev,
			       struct v4l2_subdev *subdev, bool enable)
{
	struct xvip_graph_entity *entity;

	list_for_each_entry(entity, &xdev->entities, list)
		if (of_fwnode_handle(entity->node) == subdev->fwnode) {
			bool status = entity->streaming;

			entity->streaming = enable;
			return status;
		}

	WARN(1, "Should never get here\n");
	return false;
}

static int xvip_entity_start_stop(struct xvip_composite_device *xdev, struct media_entity *entity, bool start)
{
	struct v4l2_subdev *subdev;
	bool is_streaming;
	int ret = 0;

	dev_dbg(xdev->dev, "%s entity %s\n",
		start ? "Starting" : "Stopping", entity->name);
	subdev = media_entity_to_v4l2_subdev(entity);

	/* This is to maintain list of stream on/off devices */
	is_streaming = xvip_subdev_set_streaming(xdev, subdev, start);

	/*
	 * start or stop the subdev only once in case if they are
	 * shared between sub-graphs
	 */
	if (start && !is_streaming) {
		/* power-on subdevice */
		ret = v4l2_subdev_call(subdev, core, s_power, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_power on failed on subdev\n");
			xvip_subdev_set_streaming(xdev, subdev, 0);
			return ret;
		}

		/* stream-on subdevice */
		ret = v4l2_subdev_call(subdev, video, s_stream, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_stream on failed on subdev\n");
			v4l2_subdev_call(subdev, core, s_power, 0);
			xvip_subdev_set_streaming(xdev, subdev, 0);
		}
	} else if (!start && is_streaming) {
		/* stream-off subdevice */
		ret = v4l2_subdev_call(subdev, video, s_stream, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD) {
			dev_err(xdev->dev,
				"s_stream off failed on subdev\n");
			xvip_subdev_set_streaming(xdev, subdev, 1);
		}

		/* power-off subdevice */
		ret = v4l2_subdev_call(subdev, core, s_power, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			dev_err(xdev->dev,
				"s_power off failed on subdev\n");
	}

	return ret;
}

static int xvip_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct xvip_graph_entity *entity;
	struct xvip_graph_entity *temp, *_temp;
	LIST_HEAD(ent_list);
	int ret;

	dev_dbg(g_xdev->dev, "notify complete, all subdevs registered\n");

	/* Create links for every entity. */
	list_for_each_entry(entity, &g_xdev->entities, list) {
		ret = xvip_graph_build_one(g_xdev, entity);
		if (ret < 0)
			return ret;
	}

	dev_dbg(g_xdev->dev, "Going to register v4l2 device \n");

	ret = v4l2_device_register_subdev_nodes(&g_xdev->v4l2_dev);
	if (ret < 0)
		dev_err(g_xdev->dev, "failed to register subdev nodes\n");
	
	return media_device_register(&g_xdev->media_dev);
}

static int xvip_graph_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct xvip_graph_entity *entity;

	/* Locate the entity corresponding to the bound subdev and store the
	 * subdev pointer.
	 */
	list_for_each_entry(entity, &g_xdev->entities, list) {
		if (of_fwnode_handle(entity->node) != subdev->fwnode)
			continue;

		if (entity->subdev) {
			dev_err(g_xdev->dev, "duplicate subdev for node %pOF\n",
				entity->node);
			return -EINVAL;
		}

		//dev_dbg(g_xdev->dev, "subdev %s bound\n", subdev->name);
		entity->entity = &subdev->entity;
		entity->subdev = subdev;
		return 0;
	}

	dev_err(g_xdev->dev, "no entity for subdev %s\n", subdev->name);
	return -EINVAL;
}

static const struct v4l2_async_notifier_operations xvip_graph_notify_ops = {
	.bound = xvip_graph_notify_bound,
	.complete = xvip_graph_notify_complete,
};

static int xvip_graph_parse_one(struct xvip_composite_device *xdev,
				struct device_node *node)
{
	struct xvip_graph_entity *entity;
	struct device_node *remote;
	struct device_node *ep = NULL;
	int ret = 0;

	//dev_dbg(xdev->dev, "parsing node %pOF\n", node);

	while (1) {
		ep = of_graph_get_next_endpoint(node, ep);
		if (ep == NULL)
			break;

		//dev_dbg(xdev->dev, "handling endpoint %pOF\n", ep);

		remote = of_graph_get_remote_port_parent(ep);
		if (remote == NULL) {
			ret = -EINVAL;
			break;
		}

		/* Skip entities that we have already processed. */
		if (remote == xdev->dev->of_node ||
		    xvip_graph_find_entity(xdev, remote)) {
			of_node_put(remote);
			continue;
		}

		entity = devm_kzalloc(xdev->dev, sizeof(*entity), GFP_KERNEL);
		if (entity == NULL) {
			of_node_put(remote);
			ret = -ENOMEM;
			break;
		}

		entity->node = remote;
		entity->asd.match_type = V4L2_ASYNC_MATCH_FWNODE;
		entity->asd.match.fwnode = of_fwnode_handle(remote);
		list_add_tail(&entity->list, &xdev->entities);
		xdev->num_subdevs++;
	}

	of_node_put(ep);
	return ret;
}

static int xvip_graph_parse(struct xvip_composite_device *xdev)
{
	struct xvip_graph_entity *entity;
	int ret;

	/*
	 * Walk the links to parse the full graph. Start by parsing the
	 * composite node and then parse entities in turn. The list_for_each
	 * loop will handle entities added at the end of the list while walking
	 * the links.
	 */
	ret = xvip_graph_parse_one(xdev, xdev->dev->of_node);
	if (ret < 0)
		return 0;

	list_for_each_entry(entity, &xdev->entities, list) {
		ret = xvip_graph_parse_one(xdev, entity->node);
		if (ret < 0)
			break;
	}

	return ret;
}

static void xvip_graph_cleanup(struct xvip_composite_device *xdev)
{
	struct xvip_graph_entity *entityp;
	struct xvip_graph_entity *entity;

	v4l2_async_notifier_unregister(&xdev->notifier);

	list_for_each_entry_safe(entity, entityp, &xdev->entities, list) {
		of_node_put(entity->node);
		list_del(&entity->list);
	}
}

static int xvip_graph_init(struct xvip_composite_device *xdev)
{
	struct xvip_graph_entity *entity;
	struct v4l2_async_subdev **subdevs = NULL;
	unsigned int num_subdevs;
	unsigned int i;
	int ret;

	/* Parse the graph to extract a list of subdevice DT nodes. */
	ret = xvip_graph_parse(xdev);
	if (ret < 0) {
		dev_err(xdev->dev, "graph parsing failed\n");
		goto done;
	}

	if (!xdev->num_subdevs) {
		dev_err(xdev->dev, "no subdev found in graph\n");
		goto done;
	}

	/* Register the subdevices notifier. */
	num_subdevs = xdev->num_subdevs;
	subdevs = devm_kcalloc(xdev->dev, num_subdevs, sizeof(*subdevs),
			       GFP_KERNEL);
	if (subdevs == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	i = 0;
	list_for_each_entry(entity, &xdev->entities, list)
		subdevs[i++] = &entity->asd;

	xdev->notifier.subdevs = subdevs;
	xdev->notifier.num_subdevs = num_subdevs;
	xdev->notifier.ops = &xvip_graph_notify_ops;

	ret = v4l2_async_notifier_register(&xdev->v4l2_dev, &xdev->notifier);
	if (ret < 0) {
		dev_err(xdev->dev, "notifier registration failed\n");
		goto done;
	}

	ret = 0;

done:
	if (ret < 0)
		xvip_graph_cleanup(xdev);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Controller and V4L2
 */

static void xvip_composite_v4l2_cleanup(struct xvip_composite_device *xdev)
{
	v4l2_device_unregister(&xdev->v4l2_dev);
	media_device_unregister(&xdev->media_dev);
	media_device_cleanup(&xdev->media_dev);
}

static int xvip_composite_v4l2_init(struct xvip_composite_device *xdev)
{
	int ret;

	xdev->media_dev.dev = xdev->dev;
	strlcpy(xdev->media_dev.model, "Xilinx Video Composite Device",
		sizeof(xdev->media_dev.model));
	xdev->media_dev.hw_revision = 0;

	media_device_init(&xdev->media_dev);

	xdev->v4l2_dev.mdev = &xdev->media_dev;
	ret = v4l2_device_register(xdev->dev, &xdev->v4l2_dev);
	if (ret < 0) {
		dev_err(xdev->dev, "V4L2 device registration failed (%d)\n",
			ret);
		media_device_cleanup(&xdev->media_dev);
		return ret;
	}

	return 0;
}

/* File IO stuff to start and stop stream */

/* When a process reads from our device, this gets called. */
static ssize_t device_read(struct file *flip, char *buffer, size_t len, loff_t *offset) {
	/*
	** THE START THE STREAM PART
	*/

	struct xvip_graph_entity *temp, *_temp;
	dev_dbg(g_xdev->dev, "Starting the stream \n");
	list_for_each_entry_safe(temp, _temp, &g_xdev->entities, list)
			xvip_entity_start_stop(g_xdev, temp->entity, true);

	printk(KERN_ALERT "The stream hast started.\n");
 	return 0;
}

/* Called when a process tries to write to our device */

static ssize_t device_write(struct file *flip, const char *buffer, size_t len, loff_t *offset) {
	printk(KERN_ALERT "This operation is not supported.\n");
	return 0;
}

/* Called when a process opens our device */

static int device_open(struct inode *inode, struct file *file) {
	/* If device is open, return busy */
	if (device_open_count) {
		return -EBUSY;
	}
	device_open_count++;
	try_module_get(THIS_MODULE);
	return 0;
}

/* Called when a process closes our device */

static int device_release(struct inode *inode, struct file *file) {
	 /* Decrement the open counter and usage count. Without this, the module would not unload. */
	 device_open_count--;
	 module_put(THIS_MODULE);
	 return 0;
}

struct file_operations fops = {
       read: device_read,
       write: device_write,
       open: device_open,
       release: device_release
};

/* media_ctl probe and remove */

static int media_ctl_probe(struct platform_device *pdev)
{
	/* For video4Linux */
	int ret;

	g_xdev = devm_kzalloc(&pdev->dev, sizeof(*g_xdev), GFP_KERNEL);
	if (!g_xdev)
		return -ENOMEM;

	g_xdev->dev = &pdev->dev;
	mutex_init(&g_xdev->lock);
	INIT_LIST_HEAD(&g_xdev->entities);

	ret = xvip_composite_v4l2_init(g_xdev);
	if (ret < 0)
		return ret;

	ret = xvip_graph_init(g_xdev);
	if (ret < 0)
		goto error;

	/*
	** Start the init of extra device
	*/
	
	ret = register_chrdev(0, "start_stream", &fops);
	dev_dbg(g_xdev->dev, "Registered on Major %d \n", ret);
	if (ret < 0)
		goto error;

	return 0;
	/* Error handling v4l */
error:
	xvip_composite_v4l2_cleanup(g_xdev);
	return ret;
}

static int media_ctl_remove(struct platform_device *pdev)
{
	/* Video 4 Linux cleanup */
	struct xvip_composite_device *g_xdev = platform_get_drvdata(pdev);

	mutex_destroy(&g_xdev->lock);
	xvip_graph_cleanup(g_xdev);
	xvip_composite_v4l2_cleanup(g_xdev);

	return 0;
}


static const struct of_device_id media_ctl_ids[] = {
	{ .compatible = "topic,mediactl" },
	{ },
};
MODULE_DEVICE_TABLE(of, media_ctl_ids);

static struct platform_driver media_ctl_driver = {
	.driver = {
		.name = "media_ctl",
		.owner = THIS_MODULE,
		.of_match_table = media_ctl_ids,
	},
	.probe = media_ctl_probe,
	.remove = media_ctl_remove,
};
module_platform_driver(media_ctl_driver);
