// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Watchdog driver for virtio
 *  Copyright (C) 2023 huazhiqiang
 */

#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/watchdog.h>
#include <linux/virtio.h>
#include <linux/virtio_wdt.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/slab.h>

#define VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT 30
#define VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT_MIN 1
#define VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT_MAX 600

static int timeout;	/* in seconds */
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds. (1<=timeout<="
		 __MODULE_STRING(VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT_MAX) ", default="
		 __MODULE_STRING(VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct virtio_wdt {
	struct virtio_device   *vdev;
	struct watchdog_device wdd;
	struct pci_dev         *pdev;
	/* event vq*/
	struct virtqueue       *vq;
	/* heartbeat timeout */
	unsigned int           timeout;

	/* Wait for a host response to a guest request. */
	wait_queue_head_t      evt_acked;

	spinlock_t             lock;
};

#define to_virtio_wdt(wptr) container_of(wptr, struct virtio_wdt, wdd)

static void handle_event(struct virtqueue *vq)
{
	/*host received heartbeat*/
	struct virtio_wdt *vwdt = vq->vdev->priv;

	wake_up(&vwdt->evt_acked);
}

static void virtwdt_del_vq(struct virtio_wdt *vwdt)
{
	struct virtio_device *vdev = vwdt->vdev;

	vdev->config->del_vqs(vdev);
}

static int virtwdt_init_vq(struct virtio_wdt *vwdt)
{
	int err;
	/* We expect a single virtqueue. */
	vwdt->vq = virtio_find_single_vq(vwdt->vdev, handle_event, "event");
	if (IS_ERR(vwdt->vq)) {
		err = PTR_ERR(vwdt->vq);
		goto err_vq;
	}

	return 0;
err_vq:
	return -ENOMEM;
}

static void send_event(struct virtio_wdt *vwdt, u16 type)
{
	struct scatterlist sg;
	unsigned int len;
        const struct virtio_watchdog_event evt = {
                .type = cpu_to_virtio16(vwdt->vdev, type),
        };
        sg_init_one(&sg, &evt, sizeof(evt));



	/* We should always be able to add one buffer to an empty queue. */
	virtqueue_add_outbuf(vwdt->vq, &sg, 1, vwdt, GFP_KERNEL);
	virtqueue_kick(vwdt->vq);

	/* When host has read buffer, this completes via balloon_ack */
	if (type != VIRTIO_WATCHDOG_HEARTBEAT) {
		wait_event(vwdt->evt_acked, virtqueue_get_buf(vwdt->vq, &len));
	}
}

static int virtwdt_ping(struct watchdog_device *wdd)
{
	/* sead ping to virtio watchdog device*/
	struct virtio_wdt *vwdt = to_virtio_wdt(wdd);
	send_event(vwdt, VIRTIO_WATCHDOG_HEARTBEAT);
	return 0;
}

static int virtwdt_start(struct watchdog_device *wdd)
{
	/* enable watchdog*/
	struct virtio_wdt *vwdt = to_virtio_wdt(wdd);
	send_event(vwdt, VIRTIO_WATCHDOG_ENABLE);
	return 0;
}

static int virtwdt_stop(struct watchdog_device *wdd)
{
	/* disable watchdog*/
	struct virtio_wdt *vwdt = to_virtio_wdt(wdd);
	send_event(vwdt, VIRTIO_WATCHDOG_DISABLE);
	return 0;
}

static int virtwdt_set_timeout(struct watchdog_device *wdd,
			       unsigned int new_timeout)
{
	/*TODO: set timeout to virtio watchdog device */
	struct virtio_wdt *vwdt = to_virtio_wdt(wdd);
	/* Legacy balloon config space is LE, unlike all other devices. */
	virtio_cwrite(vwdt->vdev, struct virtio_watchdog_config, timeout,
			 &new_timeout);
	return 0;
}

static const struct watchdog_info virtwdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "Virtio Watchdog",
};

static const struct watchdog_ops virtwdt_ops = {
	.owner = THIS_MODULE,
	.start = virtwdt_start,
	.stop = virtwdt_stop,
	.ping = virtwdt_ping,
	.set_timeout = virtwdt_set_timeout,
};

static int virtwdt_probe(struct virtio_device *vdev)
{
	struct virtio_wdt *vwdt;
	int err;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	vdev->priv = vwdt = kzalloc(sizeof(*vwdt), GFP_KERNEL);
	if (!vwdt)
		return -ENOMEM;

	vwdt->timeout = VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT;
	init_waitqueue_head(&vwdt->evt_acked);
	vwdt->vdev = vdev;

	err = virtwdt_init_vq(vwdt);
	if (err) {
		dev_err(&vdev->dev, "Failed to initialize vqs.\n");
		goto free;
	}

	/* Initialize the watchdog and make sure it does not run */
	vwdt->wdd.info = &virtwdt_info;
	vwdt->wdd.ops = &virtwdt_ops;
	vwdt->wdd.min_timeout = VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT_MIN;
	vwdt->wdd.max_timeout = VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT_MAX;
	vwdt->wdd.timeout = VIRTIO_WATCHDOG_HEARTBEAT_TIMEOUT;
	watchdog_init_timeout(&vwdt->wdd, timeout, NULL);
	watchdog_set_nowayout(&vwdt->wdd, nowayout);
	watchdog_stop_on_reboot(&vwdt->wdd);
	watchdog_stop_on_unregister(&vwdt->wdd);

	/* Register the watchdog so that userspace has access to it */
	err = watchdog_register_device(&vwdt->wdd);
	if (err != 0)
		goto free_dev;
	dev_info(&vwdt->vdev->dev,
		"initialized. heartbeat=%d sec (nowayout=%d)\n",
		vwdt->wdd.timeout, nowayout);

	virtio_device_ready(vdev);
	
	return 0;

free_dev:
	virtio_reset_device(vdev);
	virtwdt_del_vq(vwdt);
free:
	kfree(vwdt);
	return err;
}

static void virtwdt_remove(struct virtio_device *vdev)
{
	struct virtio_wdt *vwdt = vdev->priv;

	dev_info(&vdev->dev, "Start virtwdt_remove.\n");

	/* Now we reset the device so we can clean up the queues. */
	virtio_reset_device(vdev);
	virtwdt_del_vq(vwdt);
	kfree(vwdt);
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_WATCHDOG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_wdt_driver = {
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtwdt_probe,
	.remove =	virtwdt_remove,
};

module_virtio_driver(virtio_wdt_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio watchdog driver");
MODULE_LICENSE("GPL");

