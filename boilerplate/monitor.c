#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/slab.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 100


// ---------------- DATA STRUCT ----------------
struct monitor_entry {

    pid_t pid;
    char container_id[MONITOR_NAME_LEN];

    unsigned long soft_limit;
    unsigned long hard_limit;

    int soft_triggered;

    struct list_head list;
};


// ---------------- GLOBAL ----------------
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;


// ---------------- RSS HELPER ----------------
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);

    if (!task) {
        rcu_read_unlock();
        return -1;
    }

    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);

    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }

    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}


// ---------------- TIMER LOOP ----------------
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {

        long rss = get_rss_bytes(entry->pid);

        // process exited
        if (rss < 0) {

            printk(KERN_INFO
                   "[container_monitor] CLEANUP pid=%d\n",
                   entry->pid);

            list_del(&entry->list);
            kfree(entry);

            continue;
        }


        // -------- SOFT LIMIT --------
        if (!entry->soft_triggered &&
            rss >= entry->soft_limit) {

            printk(KERN_WARNING
                   "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
                   entry->container_id,
                   entry->pid,
                   rss,
                   entry->soft_limit);

            entry->soft_triggered = 1;
        }


        // -------- HARD LIMIT --------
        if (rss >= entry->hard_limit) {

            struct task_struct *task, *child;

            printk(KERN_WARNING
                   "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
                   entry->container_id,
                   entry->pid,
                   rss,
                   entry->hard_limit);

            rcu_read_lock();

            task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);

            if (task) {

                get_task_struct(task);

                rcu_read_unlock();

                // kill main process
                send_sig(SIGKILL, task, 0);

                // kill children
                rcu_read_lock();

                list_for_each_entry(child,
                                    &task->children,
                                    sibling) {

                    send_sig(SIGKILL, child, 0);
                }

                rcu_read_unlock();

                put_task_struct(task);
            }
            else {

                rcu_read_unlock();
            }


            list_del(&entry->list);

            kfree(entry);

            continue;
        }

    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer,
              jiffies +
              msecs_to_jiffies(CHECK_INTERVAL_MS));
}



// ---------------- IOCTL ----------------
static long monitor_ioctl(struct file *f,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req,
                       (void __user *)arg,
                       sizeof(req)))
        return -EFAULT;


    // ---------- REGISTER ----------
    if (cmd == MONITOR_REGISTER) {

        struct monitor_entry *entry, *tmp;

        mutex_lock(&monitor_lock);

        // prevent duplicates
        list_for_each_entry(tmp,
                            &monitor_list,
                            list) {

            if (tmp->pid == req.pid) {

                mutex_unlock(&monitor_lock);

                return -EEXIST;
            }
        }


        entry = kmalloc(sizeof(*entry),
                        GFP_KERNEL);

        if (!entry) {

            mutex_unlock(&monitor_lock);

            return -ENOMEM;
        }


        entry->pid = req.pid;

        strncpy(entry->container_id,
                req.container_id,
                MONITOR_NAME_LEN - 1);

        entry->container_id
            [MONITOR_NAME_LEN - 1] = '\0';


        entry->soft_limit = req.soft_limit_bytes;

        entry->hard_limit = req.hard_limit_bytes;

        entry->soft_triggered = 0;


        list_add(&entry->list,
                 &monitor_list);

        mutex_unlock(&monitor_lock);


        printk(KERN_INFO
               "[container_monitor] REGISTER container=%s pid=%d soft=%lu hard=%lu\n",
               entry->container_id,
               entry->pid,
               entry->soft_limit,
               entry->hard_limit);

        return 0;
    }


    // ---------- UNREGISTER ----------
    if (cmd == MONITOR_UNREGISTER) {

        struct monitor_entry *entry, *tmp;

        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(entry,
                                 tmp,
                                 &monitor_list,
                                 list) {

            if (entry->pid == req.pid) {

                list_del(&entry->list);

                kfree(entry);

                mutex_unlock(&monitor_lock);

                printk(KERN_INFO
                       "[container_monitor] UNREGISTER pid=%d\n",
                       req.pid);

                return 0;
            }
        }

        mutex_unlock(&monitor_lock);

        return -ENOENT;
    }


    return -EINVAL;
}



// ---------------- FILE OPS ----------------
static struct file_operations fops = {

    .owner = THIS_MODULE,

    .unlocked_ioctl = monitor_ioctl,
};



// ---------------- INIT ----------------
static int __init monitor_init(void)
{

    if (alloc_chrdev_region(&dev_num,
                            0,
                            1,
                            DEVICE_NAME) < 0)
        return -1;


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)

    cl = class_create(DEVICE_NAME);

#else

    cl = class_create(THIS_MODULE,
                      DEVICE_NAME);

#endif


    if (IS_ERR(cl)) {

        unregister_chrdev_region(dev_num, 1);

        return PTR_ERR(cl);
    }


    if (IS_ERR(device_create(cl,
                             NULL,
                             dev_num,
                             NULL,
                             DEVICE_NAME))) {

        class_destroy(cl);

        unregister_chrdev_region(dev_num, 1);

        return -1;
    }


    cdev_init(&c_dev,
              &fops);


    if (cdev_add(&c_dev,
                 dev_num,
                 1) < 0) {

        device_destroy(cl,
                       dev_num);

        class_destroy(cl);

        unregister_chrdev_region(dev_num, 1);

        return -1;
    }


    timer_setup(&monitor_timer,
                timer_callback,
                0);


    mod_timer(&monitor_timer,

              jiffies +

              msecs_to_jiffies(CHECK_INTERVAL_MS));


    printk(KERN_INFO
           "[container_monitor] Module loaded\n");


    return 0;
}



// ---------------- EXIT ----------------
static void __exit monitor_exit(void)
{
    struct monitor_entry *entry, *tmp;


    del_timer_sync(&monitor_timer);


    mutex_lock(&monitor_lock);


    list_for_each_entry_safe(entry,
                             tmp,
                             &monitor_list,
                             list) {

        list_del(&entry->list);

        kfree(entry);
    }


    mutex_unlock(&monitor_lock);


    cdev_del(&c_dev);


    device_destroy(cl,
                   dev_num);


    class_destroy(cl);


    unregister_chrdev_region(dev_num,
                             1);


    printk(KERN_INFO
           "[container_monitor] Module unloaded\n");
}


module_init(monitor_init);

module_exit(monitor_exit);


MODULE_LICENSE("GPL");

MODULE_DESCRIPTION("Container Memory Monitor - FIXED Task 4");
