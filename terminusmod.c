#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/pid.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include "../inc/terminus.h"

MODULE_LICENSE("GPL");
int mem_info_wait;

/* As named device number */
static dev_t dev_number;
static struct cdev c_dev;
static struct class *class;

/* Structure for WAIT and WAITALL*/
struct waiter {
	struct delayed_work wa_checker;
	int wa_fin;
	struct task_struct **wa_pids;	
	int wa_pids_size;
	int async;
	int sleep;
};

/* Structure for handlers */
struct handler_struct {
	int finished; 
	struct work_struct worker; /* workqueue*/
	struct list_head list; /* list used for asynchronous executions*/
	struct module_argument arg; 
	int sleep; 
	int id; 
};

static int task_id; 
LIST_HEAD(tasks); 
struct mutex glob_mut; 
static int once;

static int t_open(struct inode *i, struct file *f)
{
	return 0;
}
static int t_close(struct inode *i, struct file *f)
{
	return 0;
}
static long iohandler(struct file *filp, unsigned int cmd, unsigned long arg);
static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = t_open,
	.release = t_close,
	.unlocked_ioctl = iohandler,
};

static struct workqueue_struct *station;
DECLARE_WAIT_QUEUE_HEAD(cond_wait_queue);
static void t_wait_slow(struct work_struct *work);

static int __init start(void)
{
	int result;
	struct device *dev_return;
	task_id = 0;
	result = 0;
	mutex_init(&glob_mut);
	result = alloc_chrdev_region(&dev_number, 0, 1, "terminus");
	if (result < 0)
		goto fail;

	cdev_init(&c_dev, &fops);
	result = cdev_add(&c_dev, dev_number, 1);
	if (result < 0)
		goto fail;
	class = class_create(THIS_MODULE, "char");
	if (IS_ERR(class)) {
		result = PTR_ERR(class);
	goto fail_class;
}

dev_return = device_create(class, NULL, dev_number, NULL, "terminus");

if (IS_ERR(dev_return)) {
	result = PTR_ERR(dev_return);
	goto device_fail;
}

station = create_workqueue("workstation");

if (station == NULL)
	return -1;

pr_info("Terminal\n");
return 0;

device_fail:
	class_destroy(class);
fail_class:
	cdev_del(&c_dev);
	unregister_chrdev_region(dev_number, 1);
fail:
	return result;
}
static void __exit end(void)
{
	destroy_workqueue(station);
	pr_alert("Terminal done\n");
	device_destroy(class, dev_number);
	class_destroy(class);
	cdev_del(&c_dev);
	unregister_chrdev_region(dev_number, 1);
}

module_init(start);
module_exit(end);

static void async_janitor(struct handler_struct *handler)
{
	if (handler->arg.async)
		handler->finished = 1;
}

static void t_list(struct work_struct *work)
{
	struct handler_struct *handler;
	struct list_head *head;
	char *out = kzalloc(sizeof(char) * T_BUF_SIZE, GFP_KERNEL);
	char *aux = out;
	int inverse_sum = T_BUF_SIZE;	
	mutex_lock(&glob_mut);
	list_for_each(head, &tasks) {
	handler = list_entry(head, struct handler_struct, list);

	switch (handler->arg.arg_type) {
		case modinfo_t:
			scnprintf(aux, inverse_sum, " %d MODINFO\n ",handler->id);
			break;
		case meminfo_t:
			scnprintf(aux, inverse_sum, " %d MEMINFO\n ",handler->id);
			break;	
		case kill_t:
			scnprintf(aux, inverse_sum, " %d KILL\n ", handler->id);
			break;
		default:
		break;
					}
	aux += strlen(aux) - 1;
	inverse_sum -= strlen(aux) - 1;
}

	mutex_unlock(&glob_mut);
	handler = container_of(work, struct handler_struct, worker);
	copy_to_user(handler->arg.list_a.out, out, T_BUF_SIZE * sizeof(char));
	kfree(out);
	handler->sleep = 1;
	wake_up(&cond_wait_queue);
}

static void t_fg(struct work_struct *work)
{
	int id, done = 0;
	struct handler_struct *handler, *handler_done;
	struct list_head *head, *tmp;
	handler = container_of(work, struct handler_struct, worker);
	id = handler->arg.fg_a.id;
	mutex_lock(&glob_mut);
	if (!list_empty(&tasks)) {
		list_for_each_safe(head, tmp, &tasks) {
		handler_done =list_entry(head, struct handler_struct, list);
	if (handler_done->id == id) {
		while (!done) {
			if (handler_done->finished) {
				memcpy(handler, handler_done,sizeof(struct handler_struct));
				list_del(&(handler_done->list));
				done = 1;
			} else {
				wait_event(cond_wait_queue,handler_done->sleep!= 0);
				}
			}
		}
	}
}

	mutex_unlock(&glob_mut);
	handler->sleep = 1;
	wake_up(&cond_wait_queue);
}

static void t_kill(struct work_struct *work)
{
	struct handler_struct *handler;
	struct pid *pid_target;
	handler = container_of(work, struct handler_struct, worker);
	pid_target = find_get_pid(handler->arg.kill_a.pid);
	
	if (pid_target) {
		handler->arg.kill_a.state = 1;
		kill_pid(pid_target, handler->arg.kill_a.sig, 1);
	} else {
		handler->arg.kill_a.state = 0;
	}
		handler->sleep = 1;
		wake_up(&cond_wait_queue);
	}
static void t_wait(struct work_struct *work)
{
	struct handler_struct *handler;
	struct waiter *wtr;
	int i, killed;
	struct pid_list pidlist;
	int *tab;
	struct pid *p;
	struct pid_ret *rets;
	handler = container_of(work, struct handler_struct, worker);
	wtr = kmalloc(sizeof(struct waiter), GFP_KERNEL);
	INIT_DELAYED_WORK(&(wtr->wa_checker), t_wait_slow);
	pidlist = handler->arg.pid_list_a;

	copy_from_user(&pidlist, &handler->arg.pid_list_a,sizeof(struct pid_list));
	tab = kcalloc(pidlist.size, sizeof(int), GFP_KERNEL);
	if (tab == NULL)
		return;

	copy_from_user(tab, pidlist.first, sizeof(int) * pidlist.size);
	rets = kcalloc(pidlist.size, sizeof(struct pid_ret), GFP_KERNEL);
	wtr->wa_pids =kzalloc(sizeof(struct task_struct *) * pidlist.size, GFP_KERNEL);
	wtr->wa_pids_size = pidlist.size;

	for (i = 0; i < pidlist.size; i++) {
		p = find_get_pid(tab[i]);
		if (!p)
			goto nope_pid;
			wtr->wa_pids[i] = get_pid_task(p, PIDTYPE_PID);
		if (!wtr->wa_pids[i])
			goto nope_pid;
			put_pid(p);
		}

for (killed = 0; killed < wtr->wa_pids_size;) {
	for (i = 0; i < wtr->wa_pids_size; i++) {
		if (wtr->wa_pids[i] != NULL) {
			if (!pid_alive(wtr->wa_pids[i])) {
				killed++;
				rets[i].pid = tab[i];
				rets[i].ret =wtr->wa_pids[i]->exit_code;
				put_task_struct(wtr->wa_pids[i]);
				wtr->wa_pids[i] = NULL;
			}
		}
	}

	if (killed == wtr->wa_pids_size)
		break;

	if ((killed > 0) && (once == 1)) {
		once = 0;
		break;
	}

	if ((queue_delayed_work
		(station, &(wtr->wa_checker), DELAY)) == 0) {
				wtr->wa_fin = 0;
				pr_info("Avant wait interrupt");
				wait_event_interruptible(cond_wait_queue,
				wtr->wa_fin != 0);
		}
	}

nope_pid:
	kfree(wtr->wa_pids);
	kfree(wtr);
	kfree(tab);
	kfree(rets);
	handler->sleep = 1;
	async_janitor(handler);
	wake_up(&cond_wait_queue);
}
static void t_wait_slow(struct work_struct *work)
{
	struct waiter *wtr;
	struct delayed_work *dw;
	dw = to_delayed_work(work);
	wtr = container_of(dw, struct waiter, wa_checker);
	wtr->wa_fin = 1;
	wake_up_interruptible(&cond_wait_queue);
}
static void t_meminfo(struct work_struct *work)
{
	struct handler_struct *handler;
	handler = container_of(work, struct handler_struct, worker);
	si_meminfo((struct sysinfo *)&(handler->arg.meminfo_a));
	handler->sleep = 1;
	async_janitor(handler);
	wake_up(&cond_wait_queue);
}
static void t_modinfo(struct work_struct *work)
{
	struct handler_struct *handler;
	struct module *mod;
	int i = 0;
	char *mod_name;
	mod_name = kcalloc(T_BUF_STR, sizeof(char), GFP_KERNEL);
	handler = container_of(work, struct handler_struct, worker);

	copy_from_user(mod_name, (void *)handler->arg.modinfo_a.arg,T_BUF_STR * sizeof(char));
	mod = find_module(mod_name);
	if (mod != NULL) {
			scnprintf(handler->arg.modinfo_a.data.name, T_BUF_STR, "%s",mod->name);
			scnprintf(handler->arg.modinfo_a.data.version, T_BUF_STR, "%s",mod->version);
			handler->arg.modinfo_a.data.module_core = mod->module_core;
			handler->arg.modinfo_a.data.num_kp = mod->num_kp;
		while (i < mod->num_kp) {

			scnprintf(handler->arg.modinfo_a.data.args, T_BUF_STR,"%s ", mod->kp[i].name);
			i++;
		}
	} else {

			handler->arg.modinfo_a.data.module_core = NULL;
	}
	kfree(mod_name);
	handler->sleep = 1;
	async_janitor(handler);
	wake_up(&cond_wait_queue);
}

void do_it(struct module_argument *arg)
{
	struct handler_struct *handler;
	handler = kzalloc(sizeof(struct handler_struct), GFP_KERNEL);
	handler->sleep = 0;
	handler->id = task_id++;
	copy_from_user(&(handler->arg), arg, sizeof(struct module_argument));
	switch (arg->arg_type) {
		case meminfo_t:
			INIT_WORK(&(handler->worker), t_meminfo);
			break;
		case modinfo_t:
			INIT_WORK(&(handler->worker), t_modinfo);
			break;
		case kill_t:
			INIT_WORK(&(handler->worker), t_kill);
			break;
		case fg_t:
			INIT_WORK(&(handler->worker), t_fg);
			break;
		case pid_list_t:
			INIT_WORK(&(handler->worker), t_list);
			break;
		case wait_t:
			once = 1;
			INIT_WORK(&(handler->worker), t_wait);
			break;
		case wait_all_t:
			once = 0;
			INIT_WORK(&(handler->worker), t_wait);
			break;
		default:
			pr_info("Default case is supposed to be unreachable\n");	
			break;
}

mutex_lock(&glob_mut);
schedule_work(&(handler->worker));

	if (handler->arg.async && (arg->arg_type != fg_t)
		&&(arg->arg_type != pid_list_t)) {
		handler->finished = 0;
		list_add_tail(&(handler->list), &tasks);
		mutex_unlock(&glob_mut);
	} else {
		mutex_unlock(&glob_mut);
		wait_event(cond_wait_queue, handler->sleep != 0);
		copy_to_user((void *)arg, (void *)&(handler->arg),
		sizeof(struct module_argument));
		kfree(handler);
	}
}
long iohandler(struct file *filp, unsigned int cmd, unsigned long arg)
{

	switch (cmd) {
		case T_WAIT:
		case T_WAIT_ALL:
		case T_KILL:
		case T_MEMINFO:
		case T_MODINFO:
		case T_FG:
		case T_LIST:
			do_it((struct module_argument *)arg);
			break;
		default:
			pr_alert("Unknown command");
		return -1;
		}	
	return 0;
}

