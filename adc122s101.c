/*
  adc.c

  Copyright Don Smyth, 2013

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/kfifo.h>
#include <linux/dma-mapping.h>

/* number of messages to use */
#define NUM_MSGS 2
/* number of reads per transfer */
#define NUM_READS	PAGE_SIZE
#define SPI_BUFF_SIZE	(NUM_READS * 2)
#define USER_BUFF_SIZE	128
#define FIFO_SIZE SPI_BUFF_SIZE * NUM_MSGS * 32

/*
The McSPI controller available speeds are

48M / (1 << 0) -> 48 MHz
48M / (1 << 1) -> 24 MHz
48M / (1 << 2) -> 12 MHz
48M / (1 << 3) -> 6 MHz
48M / (1 << 4) -> 3 MHz
...
48M / (1 << 15) -> 1465 Hz

So 12 MHz is the best we can do with ADC122S101 maxing out at 16 MHz

*/
#define SPI_BUS 2
#define SPI_BUS_CS1 0

//#define BASE_BUS_SPEED 12000000 // 375000 samples/s
#define BASE_BUS_SPEED 3000000 // 93750 samples/s
//#define BASE_BUS_SPEED 1500000 // 46875 samples/s

static int bus_speed = BASE_BUS_SPEED;
module_param(bus_speed, int, S_IRUGO);
MODULE_PARM_DESC(bus_speed, "SPI bus speed in Hz");

const char this_driver_name[] = "adc";

static int running = 0;

struct adc_message {
	struct list_head list;
	struct completion completion;
	struct spi_message msg;
	struct spi_transfer transfer;
	u8 *tx_buf;
	u8 *rx_buf;
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
};

struct adc_dev {
	struct semaphore spi_sem;
	struct semaphore fop_sem;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct spi_device *spi_device;
	struct adc_message adc_msg[NUM_MSGS];
	char *user_buff;
	struct kfifo kf;
};

static struct adc_dev adc_dev;

static LIST_HEAD(done_list);
static LIST_HEAD(work_list);
static DEFINE_MUTEX(list_lock);

static void adc_async_complete(void *arg);
static int adc_async(struct adc_message *adc_msg);

static void adc_workq_handler(struct work_struct *work)
{
	struct adc_message *adc_msg;
	struct adc_message *next;
	int ret;
	static int alerted=0;

	/*
	  get everything out of the done_list and into the work_list
	  so we don't hold up adc_async_complete() with the list_lock
	*/
	mutex_lock(&list_lock);
	list_for_each_entry_safe(adc_msg, next, &done_list, list) {
		list_del_init(&adc_msg->list);
		list_add_tail(&adc_msg->list, &work_list);
	}
	mutex_unlock(&list_lock);

	/* now we can process the work_list at our leisure */
	list_for_each_entry_safe(adc_msg, next, &work_list, list) {
		list_del_init(&adc_msg->list);

		if (down_interruptible(&adc_dev.spi_sem))
			return;

		/* stuff it into the fifo */
		ret = kfifo_in(&adc_dev.kf, adc_msg->rx_buf, SPI_BUFF_SIZE);

		up(&adc_dev.spi_sem);

		if (ret != SPI_BUFF_SIZE) {
			if (!alerted) {
				printk(KERN_ALERT "%s: kfifo_in returned %d\n", __func__, ret);
				alerted = 1;
			}
		} else {
			if (alerted) {
				printk(KERN_ALERT "%s: kfifo_in returned %d\n", __func__, ret);
				alerted = 0;
			}
		}

		/* resubmit the message */
		if (running)
			if (adc_async(adc_msg))
				running = 0;
	}
}

DECLARE_WORK(spi_work, adc_workq_handler);

static void adc_async_complete(void *arg)
{
	struct adc_message *adc_msg = (struct adc_message *) arg;

	mutex_lock(&list_lock);
	list_add_tail(&adc_msg->list, &done_list);
	mutex_unlock(&list_lock);

	schedule_work(&spi_work);

	complete(&adc_msg->completion);
}

/* prepare the message ahead of time */
static int adc_init_msg(struct adc_message *adc_msg)
{
	struct spi_message *message;
	struct spi_device *spi_device;
	int i;
	/* sample ch0 then ch1 alternately in buffer */
	u8 tx_templ[4] = {0x00, 0x00, 0x08, 0x00};

	spi_device = adc_dev.spi_device;

	if (spi_device == NULL) {
		printk(KERN_ALERT "adc_async(): spi_device is NULL\n");
		return -ESHUTDOWN;
	}

	INIT_COMPLETION(adc_msg->completion);

	message = &adc_msg->msg;
	spi_message_init(message);
	/* note that tx and rx buffers need to be dma safe to use this */
	message->is_dma_mapped = 1;
	message->complete = adc_async_complete;
	message->context = adc_msg;

	memset(adc_msg->rx_buf, 0, SPI_BUFF_SIZE);
	/* set up transmit buffer to alternate ch0 & ch1 */
	for (i=0; i < SPI_BUFF_SIZE; i += sizeof(tx_templ)) {
		memcpy(&adc_msg->tx_buf[i], tx_templ, sizeof(tx_templ));
	}
	memset(&adc_msg->transfer, 0, sizeof(struct spi_transfer));
	adc_msg->transfer.tx_buf = adc_msg->tx_buf;
	adc_msg->transfer.tx_dma = adc_msg->tx_dma;
	adc_msg->transfer.rx_buf = adc_msg->rx_buf;
	adc_msg->transfer.rx_dma = adc_msg->rx_dma;
	adc_msg->transfer.len = SPI_BUFF_SIZE;

	/* we need the CS raised between each transfer (measurement) */
	//adc_msg->transfer.cs_change = 1;

	/* override the bus speed if needed */
	if (spi_device->max_speed_hz != bus_speed)
		adc_msg->transfer.speed_hz = bus_speed;

	/* put in a test delay between messages, signal analyzer debug stuff */
	/*
	if (i == NUM_TRANSFERS - 1)
		adc_msg->transfer.delay_usecs = 50;
	*/

	spi_message_add_tail(&adc_msg->transfer, message);

	return 0;
}

/* start the next transfer */
static int adc_async(struct adc_message *adc_msg)
{
	int status;
	if (down_interruptible(&adc_dev.spi_sem))
		return -EFAULT;

	/* trigger the transfer */
	status = spi_async(adc_dev.spi_device, &adc_msg->msg);

	up(&adc_dev.spi_sem);

	return status;
}

static ssize_t adc_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos)
{
	ssize_t	status;
	size_t len;
	int i;

	if(down_interruptible(&adc_dev.fop_sem))
		return -ERESTARTSYS;

	memset(adc_dev.user_buff, 0, 32);
	len = count > 8 ? 8 : count;

	if (copy_from_user(adc_dev.user_buff, buff, len)) {
		status = -EFAULT;
		goto adc_write_done;
	}

	status = count;

	/* we accept two commands, "on" or "off" and ignore anything else*/
	if (!running && !strnicmp(adc_dev.user_buff, "on", 2)) {
		/* queue up all of the messages */
		for (i=0; i<NUM_MSGS; i++) {
			status = adc_async(&adc_dev.adc_msg[i]);
			if (status) {
				printk(KERN_ALERT
					"adc_write(): adc_async() returned %d\n",
					status);
				break;
			}
		}

		running = 1;
		status = count;
	} else if (!strnicmp(adc_dev.user_buff, "off", 3)) {
		running = 0;
	}

adc_write_done:
	up(&adc_dev.fop_sem);

	return status;
}

static ssize_t adc_read(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	size_t len;
	ssize_t status = 0;
	int i, w;

	if (!buff)
		return -EFAULT;

	if (down_interruptible(&adc_dev.fop_sem))
		return -ERESTARTSYS;

	/* wait until we have a full block to transfer */
	for (i = 0; (len = kfifo_len(&adc_dev.kf)) < count; i++) {
		/* how many milliseconds until the buffer is full */
		/* 8 bytes per byte clocked in at bus speed in Hertz */ 
		w =(count-len) * 8000 / BASE_BUS_SPEED;
		printk(KERN_DEBUG "adc_read(): iter %d, len %u < count %u, sleeping %d ms\n", 
			i, len, count, w);
		msleep(w);
	}
	
	if (kfifo_to_user(&adc_dev.kf, buff, count, &len)) {
		printk(KERN_ALERT "adc_read(): kfifo_to_user() failed\n");
		status = -EFAULT;
	} else {
		*offp += len;
		status = len;
	}

	up(&adc_dev.fop_sem);

	return status;
}

static int adc_open(struct inode *inode, struct file *filp)
{
	int status = 0;

	if (down_interruptible(&adc_dev.fop_sem))
		return -ERESTARTSYS;

	if (!adc_dev.user_buff) {
		adc_dev.user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!adc_dev.user_buff)
			status = -ENOMEM;
	}

	up(&adc_dev.fop_sem);

	return status;
}

static int adc_probe(struct spi_device *spi_device)
{
	struct adc_message *adc_msg;
	int i, status = 0;

	if (down_interruptible(&adc_dev.spi_sem))
		return -EBUSY;

	adc_dev.spi_device = spi_device;

	/* allow use of coherent dma buffers */
	spi_device->dev.coherent_dma_mask = ~0;

	for (i=0; i<NUM_MSGS; i++) {
		adc_msg = &adc_dev.adc_msg[i];

		init_completion(&adc_msg->completion);

		if (!adc_msg->rx_buf) {
#if 0
			adc_msg->rx_buf = kmalloc(SPI_BUFF_SIZE * sizeof(u8), GFP_KERNEL);
#else
			adc_msg->rx_buf = dma_alloc_coherent(&spi_device->dev,
								SPI_BUFF_SIZE,
								&adc_msg->rx_dma,
								GFP_DMA);
#endif
			if (!adc_msg->rx_buf)
				status = -ENOMEM;
		}

		if (!adc_msg->tx_buf) {
#if 0
			adc_msg->tx_buf = kmalloc(SPI_BUFF_SIZE * sizeof(u8), GFP_KERNEL);
#else
			adc_msg->tx_buf = dma_alloc_coherent(&spi_device->dev,
								SPI_BUFF_SIZE,
								&adc_msg->tx_dma,
								GFP_DMA);
#endif
			if (!adc_msg->tx_buf)
				status = -ENOMEM;
		}
		status = adc_init_msg(adc_msg);
		if (status) {
			printk(KERN_ALERT
				"adc_write(): adc_init_msg() returned %d\n",
				status);
		return -EFAULT;
		}
	}

	if (kfifo_alloc(&adc_dev.kf, FIFO_SIZE, GFP_KERNEL)) {
		printk(KERN_ERR "error kfifo_alloc\n");
		status = -ENOMEM;
	}

	if (!status)
		printk(KERN_ALERT "SPI[%d] max_speed_hz %d Hz  bus_speed %d Hz\n",
			spi_device->chip_select,
			spi_device->max_speed_hz,
			bus_speed);

	up(&adc_dev.spi_sem);

	return status;
}

static int adc_remove(struct spi_device *spi_device)
{
	struct adc_message *adc_msg;
	int i;

	if (down_interruptible(&adc_dev.spi_sem))
		return -EBUSY;

	adc_dev.spi_device = NULL;
	for (i=0; i<NUM_MSGS; i++) {
		adc_msg = &adc_dev.adc_msg[i];

		if (adc_msg->tx_buf)
#if 0
			kfree(adc_msg->tx_buf);
#else
			dma_free_coherent(&spi_device->dev, SPI_BUFF_SIZE,
					adc_msg->tx_buf, adc_msg->tx_dma);
#endif

		if (adc_msg->rx_buf)
#if 0
			kfree(adc_msg->rx_buf);
#else
			dma_free_coherent(&spi_device->dev, SPI_BUFF_SIZE,
					adc_msg->rx_buf, adc_msg->rx_dma);
#endif
	}

	if (adc_dev.user_buff)
		kfree(adc_dev.user_buff);

	kfifo_free(&adc_dev.kf);

	up(&adc_dev.spi_sem);

	return 0;
}

static int __init add_adc_device_to_bus(void)
{
	struct spi_master *spi_master;
	struct spi_device *spi_device;
	struct device *pdev;
	int status;
	char buff[64];

	spi_master = spi_busnum_to_master(SPI_BUS);
	if (!spi_master) {
		printk(KERN_ALERT "spi_busnum_to_master returned NULL\n");
		return -1;
	}

	spi_device = spi_alloc_device(spi_master);

	if (!spi_device) {
		status = -1;
		printk(KERN_ALERT "spi_alloc_device() failed\n");
	}

	spi_device->chip_select = SPI_BUS_CS1;

	/* first check if the bus already knows about us */
	snprintf(buff, sizeof(buff), "%s.%u",
			dev_name(&spi_device->master->dev),
			spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);
	if (pdev) {
		/* We are not going to use this spi_device */
		spi_dev_put(spi_device);

		/*
		 * There is already a device configured for this bus.cs
		 * It is okay if it us, otherwise complain and fail.
		 */
		if (pdev->driver && pdev->driver->name
			&& strcmp(this_driver_name,
					pdev->driver->name)) {
			printk(KERN_ALERT
				"Driver [%s] already registered for %s\n",
				pdev->driver->name, buff);
			status = -1;
		}
	} else {
		spi_device->max_speed_hz = bus_speed;
		spi_device->mode = SPI_MODE_3;
		spi_device->bits_per_word = 8;
		spi_device->irq = -1;
		spi_device->controller_state = NULL;
		spi_device->controller_data = NULL;
		strlcpy(spi_device->modalias, this_driver_name,
			SPI_NAME_SIZE);

		status = spi_add_device(spi_device);
		if (status < 0) {
			spi_dev_put(spi_device);
			printk(KERN_ALERT
				"spi_add_device() failed: %d\n",
				status);
		}
	}

	put_device(&spi_master->dev);

	return status;
}

static struct spi_driver adc_spi = {
	.driver = {
		.name =	this_driver_name,
		.owner = THIS_MODULE,
	},
	.probe = adc_probe,
	.remove = __devexit_p(adc_remove),
};

static int __init adc_init_spi(void)
{
	int error;

	error = spi_register_driver(&adc_spi);
	if (error < 0) {
		printk(KERN_ALERT "spi_register_driver() failed %d\n", error);
		return -1;
	}

	error = add_adc_device_to_bus();
	if (error < 0) {
		printk(KERN_ALERT "add_adc_to_bus() failed\n");
		spi_unregister_driver(&adc_spi);
	}

	return error;
}

static const struct file_operations adc_fops = {
	.owner =	THIS_MODULE,
	.read = 	adc_read,
	.write =	adc_write,
	.open =		adc_open,
};

static int __init adc_init_cdev(void)
{
	int error;

	adc_dev.devt = MKDEV(0, 0);

	if ((error = alloc_chrdev_region(&adc_dev.devt, 0, 1,
					this_driver_name)) < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() failed: %d \n",
			error);
		return -1;
	}

	cdev_init(&adc_dev.cdev, &adc_fops);
	adc_dev.cdev.owner = THIS_MODULE;

	error = cdev_add(&adc_dev.cdev, adc_dev.devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add() failed: %d\n", error);
		unregister_chrdev_region(adc_dev.devt, 1);
		return -1;
	}

	return 0;
}

static int __init adc_init_class(void)
{
	adc_dev.class = class_create(THIS_MODULE, this_driver_name);

	if (!adc_dev.class) {
		printk(KERN_ALERT "class_create() failed\n");
		return -1;
	}

	if (!device_create(adc_dev.class, NULL, adc_dev.devt, NULL,
			this_driver_name)) {
		printk(KERN_ALERT "device_create(..., %s) failed\n",
			this_driver_name);
		class_destroy(adc_dev.class);
		return -1;
	}

	return 0;
}

static int __init adc_init(void)
{
	memset(&adc_dev, 0, sizeof(struct adc_dev));

	sema_init(&adc_dev.spi_sem, 1);
	sema_init(&adc_dev.fop_sem, 1);

	if (adc_init_cdev() < 0)
		goto fail_1;

	if (adc_init_class() < 0)
		goto fail_2;

	if (adc_init_spi() < 0)
		goto fail_3;

	return 0;

fail_3:
	device_destroy(adc_dev.class, adc_dev.devt);
	class_destroy(adc_dev.class);

fail_2:
	cdev_del(&adc_dev.cdev);
	unregister_chrdev_region(adc_dev.devt, 1);

fail_1:
	return -1;
}

static void __exit adc_exit(void)
{
	spi_unregister_device(adc_dev.spi_device);
	spi_unregister_driver(&adc_spi);

	device_destroy(adc_dev.class, adc_dev.devt);
	class_destroy(adc_dev.class);

	cdev_del(&adc_dev.cdev);
	unregister_chrdev_region(adc_dev.devt, 1);

	if (adc_dev.user_buff)
		kfree(adc_dev.user_buff);
}

module_init(adc_init);
module_exit(adc_exit);

MODULE_AUTHOR("Don Smyth");
MODULE_DESCRIPTION("SPI TI ADC122S01 driver");
MODULE_LICENSE("GPL");

