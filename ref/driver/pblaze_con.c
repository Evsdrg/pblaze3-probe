
#include "pblaze_pcie.h"
#include "pblaze_raid.h"

MODULE_DESCRIPTION("MEMBlaze Conntrol Module");
MODULE_AUTHOR("MemBlaze");
MODULE_LICENSE("GPL");

extern struct pci_driver pblaze_pcie_driver;
extern struct pblaze_raid_driver raid_drv;
extern struct list_head raid_list;
extern spinlock_t raid_list_lock;

static struct pblaze_virt_dev *
pblaze_virt_dev_get(struct pblaze_virt_dev *dev)
{
	if (dev) {
		get_device(dev->dev.parent);
		get_device(&dev->dev);
	}

	return dev;
}
EXPORT_SYMBOL(pblaze_virt_dev_get);

static void pblaze_virt_dev_put(struct pblaze_virt_dev *dev)
{
	if (dev) {
		put_device(&dev->dev);
		put_device(dev->dev.parent);
	}
}
EXPORT_SYMBOL(pblaze_virt_dev_put);

static int pblaze_virt_bus_match(struct device *dev,
				 struct device_driver *drv)
{
	struct pblaze_virt_dev *virt_dev;
	struct pblaze_virt_drv *virt_drv;

	virt_dev = container_of(dev, struct pblaze_virt_dev, dev);
	virt_drv = container_of(drv, struct pblaze_virt_drv, driver);

	return virt_dev->signature == virt_drv->signature ? 1 : 0;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
static int pblaze_virt_bus_uevent(struct device *dev, char **envp,
				  int num_envp, char *buffer,
				  int buffer_size)
#else
static int pblaze_virt_bus_uevent(struct device *dev,
				  struct kobj_uevent_env *env)
#endif
{
	return -ENODEV;
}

static int __pblaze_virt_bus_probe(struct pblaze_virt_drv *virt_drv,
				   struct pblaze_virt_dev *virt_dev)
{
	int ret = 0;

	DPRINTK(DEBUG, "__pblaze_virt_bus_probe\n");

	if (!virt_dev->driver && virt_drv->probe) {
		ret = -ENODEV;
		if (virt_dev->signature == virt_drv->signature) {
			virt_dev->is_attached = true;
			ret = virt_drv->probe(virt_dev);
		}

		if (ret >= 0) {
			virt_dev->driver = virt_drv;
			ret = 0;
		} else {
			virt_dev->is_attached = false;
		}
	}

	return ret;
}

static int pblaze_virt_bus_probe(struct device *dev)
{
	int ret;
	long timeout = 0;
	struct pblaze_virt_dev *virt_dev;
	struct pblaze_virt_drv *virt_drv;

	DPRINTK(DEBUG, "pblaze_virt_bus_probe\n");

	virt_dev = container_of(dev, struct pblaze_virt_dev, dev);
	virt_drv = container_of(dev->driver, struct pblaze_virt_drv, driver);
	pblaze_virt_dev_get(virt_dev);

	timeout = pblaze_raid_wait_status(virt_dev->parent_dev, ST_STAGE_RUN,
					  HZ * 60 * 40);
	if (timeout) {
		ret = -EBUSY;
		goto failed_wait_online;
	}

	pb_atomic_inc(&virt_dev->parent_dev->outstanding_handle);

	ret = __pblaze_virt_bus_probe(virt_drv, virt_dev);
	if (ret) {
		pb_atomic_dec(&virt_dev->parent_dev->outstanding_handle);
		pblaze_virt_dev_put(virt_dev);
	}

	return 0;

failed_wait_online:
	pblaze_virt_dev_put(virt_dev);
	return ret;
}

static int pblaze_virt_bus_remove(struct device *dev)
{
	struct pblaze_virt_dev *virt_dev;
	struct pblaze_virt_drv *virt_drv;

	virt_dev = container_of(dev, struct pblaze_virt_dev, dev);
	virt_drv = virt_dev->driver;

	if (virt_drv) {
		virt_dev->is_attached = false;
		if (virt_drv->remove)
			virt_drv->remove(virt_dev);
		virt_dev->driver = NULL;
	}

	pb_atomic_dec(&virt_dev->parent_dev->outstanding_handle);
	pblaze_virt_dev_put(virt_dev);

	return 0;
}

static void pblaze_virt_bus_shutdown(struct device *dev)
{
	struct pblaze_virt_dev *virt_dev;
	struct pblaze_virt_drv *virt_drv;

	virt_dev = container_of(dev, struct pblaze_virt_dev, dev);
	virt_drv = virt_dev->driver;
	if (virt_drv && virt_drv->shutdown)
		virt_drv->shutdown(virt_dev);
}

void pblaze_virt_bus_release(struct device *dev)
{
	DPRINTK(DEBUG, "++\n");
}

struct bus_type pblaze_virt_bus_type = {
	.name = "vbus",
	.match = pblaze_virt_bus_match,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 11)
	.uevent = pblaze_virt_bus_uevent,
	.probe = pblaze_virt_bus_probe,
	.remove = pblaze_virt_bus_remove,
	.shutdown = pblaze_virt_bus_shutdown,
#else
	.hotplug = pblaze_virt_bus_uevent,
#endif
	.dev_attrs = NULL,
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 12, 0)
	.bus_attrs = NULL,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
	.pm = NULL,
#endif
};
EXPORT_SYMBOL(pblaze_virt_bus_type);

struct pblaze_virt_dev vbus0_dev = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
	.dev.init_name = "vbus0",
#else
	.dev.bus_id = "vbus0",
#endif
	.dev.bus = &pblaze_virt_bus_type,
	.dev.release = pblaze_virt_bus_release,
};

int pblaze_virt_bus_register(void)
{
	int ret;

	ret = bus_register(&pblaze_virt_bus_type);
	if (ret != 0)
		goto failed_bus_register;

	ret = device_register(&vbus0_dev.dev);
	if (ret != 0)
		goto failed_device_register;

	return 0;

failed_device_register:
	bus_unregister(&pblaze_virt_bus_type);
failed_bus_register:
	return ret;
}

void pblaze_virt_bus_unregister(void)
{
	device_unregister(&vbus0_dev.dev);
	bus_unregister(&pblaze_virt_bus_type);
}

void pblaze_virt_dev_release(struct device *dev)
{
	struct pblaze_virt_dev *virt_dev;

	DPRINTK(DEBUG, "virtual device release\n");

	virt_dev = container_of(dev, struct pblaze_virt_dev, dev);
	kfree(virt_dev);
}

int pblaze_virt_dev_register(struct pblaze_raid *raid, int minor)
{
	int ret;

	raid->virt_dev = kmalloc(sizeof(struct pblaze_virt_dev), GFP_KERNEL);
	if (!raid->virt_dev)
		return -ENOMEM;

	memset(raid->virt_dev, 0, sizeof(struct pblaze_virt_dev));
	raid->virt_dev->dev.parent = &vbus0_dev.dev;
	raid->virt_dev->dev.bus = &pblaze_virt_bus_type;
	raid->virt_dev->dev.release = pblaze_virt_dev_release;

	raid->virt_dev->signature = PB_SIGNATURE;
	raid->virt_dev->parent_dev = raid;
	raid->virt_dev->minor = minor;

	snprintf(raid->virt_dev->name, 256, "vbus0%c", (char)('a' + minor));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
	raid->virt_dev->dev.init_name = raid->virt_dev->name;
#else
	strncpy(raid->virt_dev->dev.bus_id, raid->virt_dev->name, BUS_ID_SIZE);
#endif

	down(&raid_drv.sem);
	ret = device_register(&raid->virt_dev->dev);
	if (!ret)
		raid_drv.virt_devs[minor] = raid->virt_dev;
	up(&raid_drv.sem);

	return ret;
}
EXPORT_SYMBOL(pblaze_virt_dev_register);

void pblaze_virt_dev_unregister(struct pblaze_raid *raid)
{
	down(&raid_drv.sem);
	raid_drv.virt_devs[raid->virt_dev->minor] = NULL;
	up(&raid_drv.sem);

	device_unregister(&raid->virt_dev->dev);
}
EXPORT_SYMBOL(pblaze_virt_dev_unregister);

int pblaze_virt_drv_register(struct pblaze_virt_drv *virt_drv,
			     struct module *owner, const char *mod_name)
{
	int ret;

	virt_drv->driver.bus = &pblaze_virt_bus_type;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
	virt_drv->driver.owner = owner;
	virt_drv->driver.name = virt_drv->name;
#else
	virt_drv->driver.name = (char *)driver->name;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)
	virt_drv->driver.mod_name = mod_name;
#endif

#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 9)
	{
		struct device_driver *p;

		p = &virt_drv->driver;
		p->probe = pblaze_virt_bus_probe;
		p->remove = pblaze_virt_bus_remove;
		p->shutdown = pblaze_virt_bus_shutdown;
	}
#endif

	/* register with core */
	down(&raid_drv.sem);
	ret = driver_register(&virt_drv->driver);
	up(&raid_drv.sem);

	return ret;
}
EXPORT_SYMBOL(pblaze_virt_drv_register);

void pblaze_virt_drv_unregister(struct pblaze_virt_drv *virt_drv)
{
	driver_unregister(&virt_drv->driver);
}
EXPORT_SYMBOL(pblaze_virt_drv_unregister);

struct semaphore *pblaze_virt_drv_get_lock(void)
{
	return &raid_drv.sem;
}
EXPORT_SYMBOL(pblaze_virt_drv_get_lock);


struct pblaze_virt_dev *pblaze_get_virt_dev(int minor)
{
	return raid_drv.virt_devs[minor];
}
EXPORT_SYMBOL(pblaze_get_virt_dev);

sector_t pblaze_virt_dev_get_size(struct pblaze_virt_dev *virt_dev)
{
	sector_t size = virt_dev->parent_dev->disk_size_MB;
	sector_t cap = virt_dev->parent_dev->disk_capacity_MB;

	/* The last block is for dre log page */
	if (size < cap)
		return ((size << PB_B2MB_SHIFT) - 1) << PB_B2S_SHIFT;
	else
		return ((cap << PB_B2MB_SHIFT) - 1) << PB_B2S_SHIFT;
}
EXPORT_SYMBOL(pblaze_virt_dev_get_size);

kmem_cache_t *pblaze_get_ioreq_slab(void)
{
	return raid_drv.ioreq_slab;
}
EXPORT_SYMBOL(pblaze_get_ioreq_slab);

u32 *pblaze_get_driver_size(void)
{
	return &raid_drv.driver_mem_size;
}
EXPORT_SYMBOL(pblaze_get_driver_size);

static int memcon_init(void)
{
	int ret;

	memset(&raid_drv, 0, sizeof(raid_drv));
	raid_drv.driver_mem_size = 53872u;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	raid_drv.ioreq_slab = kmem_cache_create("io_request",
		sizeof(struct io_request), 0, SLAB_HWCACHE_ALIGN, NULL, NULL);
#else
	raid_drv.ioreq_slab = kmem_cache_create("io_request",
		sizeof(struct io_request), 0, SLAB_HWCACHE_ALIGN, NULL);
#endif
	if (!raid_drv.ioreq_slab) {
		DPRINTK(ERR, "memcon init: ioreq slab init failed\n");
		ret = -ENOMEM;
		goto failed_create_ioreq_slab;
	}

	ret = pblaze_virt_bus_register();
	if (ret) {
		DPRINTK(ERR, "memcon: unable to register bus\n");
		goto failed_register_virbus;
	}

	raid_drv.major = register_blkdev(raid_drv.major, "memcon");
	if (unlikely(raid_drv.major < 0)) {
		DPRINTK(ERR, "memcon: unable to get major number\n");
		ret = raid_drv.major;
		goto failed_register_blkdev;
	}

	init_MUTEX(&raid_drv.sem);
	INIT_LIST_HEAD(&raid_list);
	spin_lock_init(&raid_list_lock);

	ret = pci_register_driver(&pblaze_pcie_driver);
	if (ret < 0) {
		DPRINTK(ERR, "pci_register_driver failed\n");
		goto failed_register_driver;
	}

	DPRINTK(DEBUG, "memcon init success\n");
	return 0;

failed_register_driver:
	unregister_blkdev(raid_drv.major, "memcon");
failed_register_blkdev:
	pblaze_virt_bus_unregister();
failed_register_virbus:
	kmem_cache_destroy(raid_drv.ioreq_slab);
failed_create_ioreq_slab:
	return ret;
}

static void memcon_exit(void)
{
	pci_unregister_driver(&pblaze_pcie_driver);
	unregister_blkdev(raid_drv.major, "memcon");
	pblaze_virt_bus_unregister();
	kmem_cache_destroy(raid_drv.ioreq_slab);
}

module_init(memcon_init);
module_exit(memcon_exit);
