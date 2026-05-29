#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "bh1750"
#define CLASS_NAME  "bh1750_class"

static dev_t dev_num;
static struct cdev bh1750_cdev;
static struct class *bh1750_class;
static struct device *bh1750_device;
static unsigned int light_value = 1000;

static int bh1750_open(struct inode *inode, struct file *filp)
{
    pr_info("bh1750: device opened\n");
    return 0;
}

static int bh1750_release(struct inode *inode, struct file *filp)
{
    pr_info("bh1750: device closed\n");
    return 0;
}

static ssize_t bh1750_read(struct file *filp, char __user *buf,
                           size_t count, loff_t *offset)
{
    char val_str[16];
    int len;
    if (*offset) return 0;
    len = snprintf(val_str, sizeof(val_str), "%u lux\n", light_value);
    if (copy_to_user(buf, val_str, len)) return -EFAULT;
    *offset = len;
    return len;
}

static ssize_t bh1750_write(struct file *filp, const char __user *buf,
                            size_t count, loff_t *offset)
{
    char val_str[16];
    if (count >= sizeof(val_str)) count = sizeof(val_str) - 1;
    if (copy_from_user(val_str, buf, count)) return -EFAULT;
    val_str[count] = '\0';
    if (sscanf(val_str, "%u", &light_value) == 1)
        pr_info("bh1750: light value set to %u lux\n", light_value);
    return count;
}

static const struct file_operations bh1750_fops = {
    .owner          = THIS_MODULE,
    .open           = bh1750_open,
    .release        = bh1750_release,
    .read           = bh1750_read,
    .write          = bh1750_write,
};

static int virtual_bh1750_probe(struct i2c_client *client,
                                const struct i2c_device_id *id)
{
    pr_info("bh1750: probed on i2c addr 0x%02x\n", client->addr);

    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

    cdev_init(&bh1750_cdev, &bh1750_fops);
    if (cdev_add(&bh1750_cdev, dev_num, 1) < 0) goto err_region;

    bh1750_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(bh1750_class)) goto err_cdev;

    bh1750_device = device_create(bh1750_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(bh1750_device)) goto err_class;

    pr_info("bh1750: driver loaded, /dev/%s created\n", DEVICE_NAME);
    return 0;

err_class:
    class_destroy(bh1750_class);
err_cdev:
    cdev_del(&bh1750_cdev);
err_region:
    unregister_chrdev_region(dev_num, 1);
    return -1;
}

static int virtual_bh1750_remove(struct i2c_client *client)
{
    device_destroy(bh1750_class, dev_num);
    class_destroy(bh1750_class);
    cdev_del(&bh1750_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("bh1750: driver removed\n");
    return 0;
}

static const struct i2c_device_id virtual_bh1750_id[] = {
    { "virtual_bh1750", 0 }, { }
};
MODULE_DEVICE_TABLE(i2c, virtual_bh1750_id);

static struct i2c_driver virtual_bh1750_driver = {
    .driver = { .name = "virtual_bh1750", .owner = THIS_MODULE },
    .probe = virtual_bh1750_probe,
    .remove = virtual_bh1750_remove,
    .id_table = virtual_bh1750_id,
};

module_i2c_driver(virtual_bh1750_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual BH1750 I2C Driver");