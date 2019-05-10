#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include "../inc/terminus.h"
#define T_PATH "/dev/terminus"
char *user_strings[T_BUF_STR];
ssize_t prompt_user(char *string, size_t count)
{
	printf("> ");
	fflush(stdout);
	return read(STDIN_FILENO, string, count);
}
void list_commandes()
{
	printf("modinfo [module]: kernel module info\n"
		"meminfo: memory info\n"
		"kill [pid] [signal]: send a signal to a process\n"
		"wait [pid] ([pid2], ...): wait for the end of one of the pid\n"
		"waitall [pid] ([pid2], ...): wait for the end of all pid\n"
		"jobs: list of asynchronous jobs still not recovered\n"
		"fg [job_id]: recovery of the results of a job_id job\n"
);
}
size_t lazy_cmp(char *s1, char *s2) {
	return strncmp(s1, s2, strlen(s2));
}
void show_results(struct module_argument arg)
{
	switch(arg.arg_type) {
		case modinfo_t:
			printf("Name\t%s\nVersion\t%s\nCore\t%p\n%d arguments",
			arg.modinfo_a.data.name,
			arg.modinfo_a.data.version,
			arg.modinfo_a.data.module_core,
			arg.modinfo_a.data.num_kp);
			if (arg.modinfo_a.data.num_kp) {
				printf(":\n%s", arg.modinfo_a.data.args);
			}
			printf("\n");
			break;
		case meminfo_t:
			printf("TotalRam\t%llu pages\n", arg.meminfo_a.totalram);
			printf("SharedRam\t%llu pages\n", arg.meminfo_a.sharedram);
			printf("FreeRam\t\t%llu pages\n", arg.meminfo_a.freeram);
			printf("BufferRam\t%llu pages\n", arg.meminfo_a.bufferram);
			printf("TotalHigh\t%llu pages\n", arg.meminfo_a.totalhigh);
			printf("FreeHigh\t%llu pages\n", arg.meminfo_a.freehigh);
			printf("Memory unit\t%lu bytes\n", arg.meminfo_a.mem_unit);
			break;		
		default:
			break;
		}
	return;
}
void meminfo(int fd, int async)
{
	struct module_argument arg;
	arg.arg_type = meminfo_t;
	arg.async = async;
	if (ioctl(fd, T_MEMINFO, &arg) == 0) {
		if (arg.async) return;
		else show_results(arg);
	} else perror("ioctl");
}
void modinfo(int fd, char* module_name, int async)
{
	int i;
	char *tmp_ptr = NULL;
	struct module_argument arg;
	arg.arg_type = modinfo_t;
	arg.async = async;
	memset(&arg.modinfo_a, 0, sizeof(union arg_infomod));
	arg.modinfo_a.arg = (char *) malloc(T_BUF_STR * sizeof(char));
	memset(arg.modinfo_a.arg, 0, T_BUF_STR);
	if (module_name == NULL) {
		printf("You must provide a module name.\n");
		free(tmp_ptr);
		return;
}
strcpy(arg.modinfo_a.arg, module_name);
for (i=0; i<T_BUF_STR; i++)
	if (arg.modinfo_a.arg[i] == '\n')
		arg.modinfo_a.arg[i] = '\0';
		tmp_ptr = arg.modinfo_a.arg;
	if (ioctl(fd, T_MODINFO, &arg) == 0) {
	/* Direct return if asynchronous. */
	if (arg.async == 1) {
		free(tmp_ptr);
		return;
	}
	if (arg.modinfo_a.data.module_core == NULL) {
		printf("Module %s not found.\n", module_name);
		free(tmp_ptr);
		return;
	}
	else {
		show_results(arg);
		free(tmp_ptr);
		return;
	}
	} else perror("ioctl");
		free(tmp_ptr);
}
void kill(int fd, char* pid, char* sig, int async)
{
	struct module_argument arg;
	arg.arg_type = kill_t;
	arg.async = async;
	if ((pid == NULL) || (sig == NULL)) {
		printf("You have to provide a pid and a signal\n");
		return;
	}
		arg.kill_a.pid = atoi(pid);
		arg.kill_a.sig = atoi(sig);
	if (ioctl(fd, T_KILL, &arg) != 0)
		perror("ioctl");
	}
void t_wait(int fd, int wait_all)
{
	struct module_argument arg;
	int i;
	if (wait_all)
		arg.arg_type = wait_all_t;
	else
		arg.arg_type = wait_t;
		arg.async = 0;	
		arg.pid_list_a.size = 0;
		arg.pid_list_a.first = NULL;
	for (i=1; user_strings[i] != NULL; i++)
		arg.pid_list_a.size++;
		if (arg.pid_list_a.size == 0) {
			printf("It takes at least one pid.\n");
			return;
		}

	arg.pid_list_a.first = (int *) malloc(arg.pid_list_a.size * sizeof(int));
	arg.pid_list_a.ret = (struct pid_ret *) malloc(arg.pid_list_a.size * sizeof(struct pid_ret));
	for (i=0; i < arg.pid_list_a.size; i++) {
		arg.pid_list_a.first[i] = atoi(user_strings[i+1]);
		}
	if (wait_all) {
		if (ioctl(fd, T_WAIT_ALL, &arg) != 0) {
			perror("ioctl");
			return;
		}
		} else {
		if (ioctl(fd, T_WAIT, &arg) != 0) {
			perror("ioctl");
			return;
			}
		}
}
void t_fg(int fd, char* id)
{
	struct module_argument arg;
	arg.arg_type = fg_t;
	arg.async = 0;
	if (user_strings[1] == NULL) {
		return;
	}
	else {
		arg.fg_a.id = atoi(user_strings[1]);
	}
	if (ioctl(fd, T_FG, &arg) != 0) {
		perror("ioctl");
	}
	else {
		show_results(arg);
	}
}
void t_list(int fd)
{
	struct module_argument arg;
	arg.arg_type = pid_list_t;
	arg.async = 0;
	arg.list_a.out = (char *) malloc(T_BUF_SIZE * sizeof(char));
	arg.list_a.size = T_BUF_SIZE;
	if (ioctl(fd, T_LIST, &arg) != 0) {
		perror("ioctl");
		return;
	} else {

		printf("%s", arg.list_a.out);
	}
	return;
}
int main(int argc, char ** argv)
{
	int fd = 0;
	int i, async = 0;
	char user_string[T_BUF_STR];
	fd = open(T_PATH, O_RDWR);
	if (fd == -1) {
		perror("open");
	if (errno == ENOENT)
		printf("The module should be loaded before.\n");
		exit(EXIT_FAILURE);
	}
	memset(user_string, 0, T_BUF_STR);
	memset(user_strings, 0, T_BUF_STR);
	while (prompt_user(user_string, T_BUF_STR) > 0) {
		user_strings[0] = strtok(user_string, " ");
			for (i=1; (user_strings[i] = strtok(NULL, " ")) != NULL; i++);
				if ((user_strings[i-1]) && (*user_strings[i-1] == '&'))
					async = 1;
				if (lazy_cmp(user_strings[0], "help") == 0) {
					list_commandes();
					goto cleanup;
				}
				if (lazy_cmp(user_strings[0], "meminfo") == 0) {
					meminfo(fd, async);
					goto cleanup;
				}
				if (lazy_cmp(user_strings[0], "modinfo") == 0) {
					modinfo(fd, user_strings[1], async);
					goto cleanup;
				}
				if (lazy_cmp(user_strings[0], "kill") == 0) {
					kill(fd, user_strings[1], user_strings[2], async);
					goto cleanup;
				}
				if (lazy_cmp(user_strings[0], "waitall") == 0) {
					t_wait(fd, 1);
					goto cleanup;	
				}
				if (lazy_cmp(user_strings[0], "wait") == 0) {
					t_wait(fd, 0);
					goto cleanup;
				}
				if (lazy_cmp(user_strings[0], "fg") == 0) {
					printf("sending fg\n");
					t_fg(fd, user_strings[1]);
					goto cleanup;
				}	
				if (lazy_cmp(user_strings[0], "list") == 0) {
					t_list(fd);
					goto cleanup;
				}
			printf("usage: %s commande [args]\n", argv[0]);
			printf("help for the list of orders\n");
	cleanup:
		memset(user_string, 0, T_BUF_STR);
		memset(user_strings, 0, T_BUF_STR);
		async = 0;
	}
		printf("\n");
		close(fd);
		return EXIT_SUCCESS;
}
