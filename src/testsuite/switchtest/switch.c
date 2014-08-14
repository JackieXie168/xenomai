#include <xeno_config.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <semaphore.h>

#include <asm/xenomai/fptest.h>
#include <rtdm/rttesting.h>

#ifdef HAVE_RECENT_SETAFFINITY
#define do_sched_setaffinity(pid,len,mask) sched_setaffinity(pid,len,mask)
#else /* !HAVE_RECENT_SETAFFINITY */
#ifdef HAVE_OLD_SETAFFINITY
#define do_sched_setaffinity(pid,len,mask) sched_setaffinity(pid,mask)
#else /* !HAVE_OLD_SETAFFINITY */
typedef unsigned long cpu_set_t;
#define do_sched_setaffinity(pid,len,mask) 0
#define	 CPU_ZERO(set)		do { *(set) = 0; } while(0)
#define	 CPU_SET(n,set) 	do { *(set) |= (1 << n); } while(0)
#endif /* HAVE_OLD_SETAFFINITY */
#endif /* HAVE_RECENT_SETAFFINITY */

struct cpu_tasks;

struct task_params {
	unsigned type;
	unsigned fp;
	pthread_t thread;
	struct cpu_tasks *cpu;
	struct rttst_swtest_task swt;
};

struct cpu_tasks {
	unsigned index;
	struct task_params *tasks;
	unsigned tasks_count;
	unsigned capacity;
	unsigned fd;
};

/* Thread type. */
typedef enum {
	SLEEPER = 0,
	RTK  = 1,	 /* kernel-space thread. */
	RTUP = 2,	 /* user-space real-time thread in primary mode. */
	RTUS = 3,	 /* user-space real-time thread in secondary mode. */
	RTUO = 4,	 /* user-space real-time thread oscillating
			    between primary and secondary mode. */
} threadtype;

typedef enum {
	AFP  = 1,	 /* arm the FPU task bit (only make sense for RTK) */
	UFPP = 2,	 /* use the FPU while in primary mode. */
	UFPS = 4	 /* use the FPU while in secondary mode. */
} fpflags;

static sem_t sleeper_start;

static void timespec_substract(struct timespec *result,
			const struct timespec *lhs,
			const struct timespec *rhs)
{
	result->tv_sec = lhs->tv_sec - rhs->tv_sec;
	if (lhs->tv_nsec >= rhs->tv_nsec)
		result->tv_nsec = lhs->tv_nsec - rhs->tv_nsec;
	else {
		result->tv_sec -= 1;
		result->tv_nsec = lhs->tv_nsec + (1000000000 - rhs->tv_nsec);
	}
}

static char *task_name(struct cpu_tasks *cpu, unsigned task)
{
	char *basename [] = {
		[SLEEPER] = "sleeper",
		[RTK] = "rtk",
		[RTUP] = "rtup",
		[RTUS] = "rtus",
		[RTUO] = "rtuo"
	};
	struct task_params *param;
	static char buffer[64];
	
	if (task >= cpu->tasks_count)
		return "???";

	param = &cpu->tasks[task];
	snprintf(buffer, sizeof(buffer), "%s%u/%u",
		 basename[param->type], param->swt.index, cpu->index);

	return buffer;
}

static void handle_bad_fpreg(struct cpu_tasks *cpu, unsigned fp_val)
{
	struct rttst_swtest_error err;
	unsigned from, to;

	ioctl(cpu->fd, RTTST_RTIOC_SWTEST_GET_LAST_ERROR, &err);

	if (fp_val == ~0)
		fp_val = err.fp_val;

	from = err.last_switch.from;
	to = err.last_switch.to;

	fprintf(stderr, "Error after context switch from task %d(%s) to"
		" task %d(%s),\nFPU registers were set to %d (maybe task %s)\n",
		from, task_name(cpu, from),
		to, task_name(cpu, to),
		fp_val, task_name(cpu, fp_val % 1000));
	pthread_kill(pthread_self(), SIGSTOP);
}

static void *sleeper(void *cookie)
{
	struct task_params *param = (struct task_params *) cookie;
	unsigned tasks_count = param->cpu->tasks_count;
	struct timespec ts, last;
	int fd = param->cpu->fd;
	struct rttst_swtest_dir rtsw;
	cpu_set_t cpu_set;
	unsigned i = 0;

	CPU_ZERO(&cpu_set);
	CPU_SET(param->cpu->index, &cpu_set);
	if (do_sched_setaffinity(0, sizeof(cpu_set), &cpu_set)) {
		perror("sleeper: sched_setaffinity");
		exit(EXIT_FAILURE);
	}

	rtsw.from = param->swt.index;
	rtsw.to = param->swt.index;

	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;

	__real_sem_wait(&sleeper_start);

	clock_gettime(CLOCK_REALTIME, &last);

	/* ioctl is not a cancellation point, but we want cancellation to be
	   allowed  when suspended in ioctl. */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	for (;;) {
		struct timespec now, diff;
		unsigned expected, fp_val;
		int err;

		__real_nanosleep(&ts, NULL);

		clock_gettime(CLOCK_REALTIME, &now);

		timespec_substract(&diff, &now, &last);
		if (diff.tv_sec >= 1) {
			unsigned long switches_count;
			last = now;

			if (ioctl(fd,
				  RTTST_RTIOC_SWTEST_GET_SWITCHES_COUNT,
				  &switches_count)) {
				perror("sleeper: ioctl(RTTST_RTIOC_SWTEST_"
				       "GET_SWITCHES_COUNT)");
				exit(EXIT_FAILURE);
			}

			printf("cpu %u: %lu\n",
			       param->cpu->index,
			       switches_count);
		}

		if (tasks_count == 1)
			continue;
	
		if (++rtsw.to == rtsw.from)
			++rtsw.to;
		if (rtsw.to > tasks_count - 1)
			rtsw.to = 0;
		if (rtsw.to == rtsw.from)
			++rtsw.to;

		expected = rtsw.from + i * 1000;
		fp_regs_set(expected);
		err = ioctl(fd, RTTST_RTIOC_SWTEST_SWITCH_TO, &rtsw);
		while (err == -1 && errno == EINTR)
			err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);

		switch (err) {
		case 0:
			break;
		case 1:
			handle_bad_fpreg(param->cpu, ~0);
		case -1:
			pthread_exit(NULL);
		}
		fp_val = fp_regs_check(expected);
		if (fp_val != expected)
			handle_bad_fpreg(param->cpu, fp_val);

		if(++i == 4000000)
			i = 0;
	}

	return NULL;
}

static void *rtup(void *cookie)
{
	struct task_params *param = (struct task_params *) cookie;
	unsigned tasks_count = param->cpu->tasks_count;
	int err, fd = param->cpu->fd;
	struct rttst_swtest_dir rtsw;
	cpu_set_t cpu_set;
	unsigned i = 0;

	CPU_ZERO(&cpu_set);
	CPU_SET(param->cpu->index, &cpu_set);
	if (do_sched_setaffinity(0, sizeof(cpu_set), &cpu_set)) {
		perror("rtup: sched_setaffinity");
		exit(EXIT_FAILURE);
	}

	rtsw.from = param->swt.index;
	rtsw.to = param->swt.index;

	/* ioctl is not a cancellation point, but we want cancellation to be
	   allowed when suspended in ioctl. */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	if ((err = pthread_set_mode_np(PTHREAD_PRIMARY, 0))) {
		fprintf(stderr,
			"rtup: pthread_set_mode_np: %s\n",
			strerror(err));
		exit(EXIT_FAILURE);
	}    

	do {
		err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);
	} while (err == -1 && errno == EINTR);

	if (err == -1)
		return NULL;

	for (;;) {
		unsigned expected, fp_val;

		if (++rtsw.to == rtsw.from)
			++rtsw.to;
		if (rtsw.to > tasks_count - 1)
			rtsw.to = 0;
		if (rtsw.to == rtsw.from)
			++rtsw.to;

		expected = rtsw.from + i * 1000;
		if (param->fp & UFPP)
			fp_regs_set(expected);
		err = ioctl(fd, RTTST_RTIOC_SWTEST_SWITCH_TO, &rtsw);
		while (err == -1 && errno == EINTR)
			err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);

		switch (err) {
		case 0:
			break;
		case 1:
			handle_bad_fpreg(param->cpu, ~0);
		case -1:
			pthread_exit(NULL);
		}
		if (param->fp & UFPP) {
			fp_val = fp_regs_check(expected);
			if (fp_val != expected)
				handle_bad_fpreg(param->cpu, fp_val);
		}

		if(++i == 4000000)
			i = 0;
	}

	return NULL;
}

static void *rtus(void *cookie)
{
	struct task_params *param = (struct task_params *) cookie;
	unsigned tasks_count = param->cpu->tasks_count;
	int err, fd = param->cpu->fd;
	struct rttst_swtest_dir rtsw;
	cpu_set_t cpu_set;
	unsigned i = 0;

	CPU_ZERO(&cpu_set);
	CPU_SET(param->cpu->index, &cpu_set);
	if (do_sched_setaffinity(0, sizeof(cpu_set), &cpu_set)) {
		perror("rtus: sched_setaffinity");
		exit(EXIT_FAILURE);
	}

	rtsw.from = param->swt.index;
	rtsw.to = param->swt.index;

	/* ioctl is not a cancellation point, but we want cancellation to be
	   allowed when suspended in ioctl. */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	if ((err = pthread_set_mode_np(0, PTHREAD_PRIMARY))) {
		fprintf(stderr,
			"rtus: pthread_set_mode_np: %s\n",
			strerror(err));
		exit(EXIT_FAILURE);
	}

	do {
		err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);
	} while (err == -1 && errno == EINTR);

	if (err == -1)
		return NULL;

	for (;;) {
		unsigned expected, fp_val;

		if (++rtsw.to == rtsw.from)
			++rtsw.to;
		if (rtsw.to > tasks_count - 1)
			rtsw.to = 0;
		if (rtsw.to == rtsw.from)
			++rtsw.to;

		expected = rtsw.from + i * 1000;
		if (param->fp & UFPS)
			fp_regs_set(expected);
		err = ioctl(fd, RTTST_RTIOC_SWTEST_SWITCH_TO, &rtsw);
		while (err == -1 && errno == EINTR)
			err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);

		switch (err) {
		case 0:
			break;
		case 1:
			handle_bad_fpreg(param->cpu, ~0);
		case -1:
			pthread_exit(NULL);
		}
		if (param->fp & UFPS) {
			fp_val = fp_regs_check(expected);
			if (fp_val != expected)
				handle_bad_fpreg(param->cpu, fp_val);
		}

		if(++i == 4000000)
			i = 0;
	}

	return NULL;
}

static void *rtuo(void *cookie)
{
	struct task_params *param = (struct task_params *) cookie;
	unsigned mode, tasks_count = param->cpu->tasks_count;
	int err, fd = param->cpu->fd;
	struct rttst_swtest_dir rtsw;
	cpu_set_t cpu_set;
	unsigned i = 0;

	CPU_ZERO(&cpu_set);
	CPU_SET(param->cpu->index, &cpu_set);
	if (do_sched_setaffinity(0, sizeof(cpu_set), &cpu_set)) {
		perror("rtuo: sched_setaffinity");
		exit(EXIT_FAILURE);
	}

	rtsw.from = param->swt.index;
	rtsw.to = param->swt.index;

	/* ioctl is not a cancellation point, but we want cancellation to be
	   allowed when suspended in ioctl. */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	if ((err = pthread_set_mode_np(PTHREAD_PRIMARY, 0))) {
		fprintf(stderr,
			"rtup: pthread_set_mode_np: %s\n",
			strerror(err));
		exit(EXIT_FAILURE);
	}
	do {
		err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);
	} while (err == -1 && errno == EINTR);

	if (err == -1)
		return NULL;

	mode = PTHREAD_PRIMARY;
	for (;;) {
		unsigned expected, fp_val;

		if (++rtsw.to == rtsw.from)
			++rtsw.to;
		if (rtsw.to > tasks_count - 1)
			rtsw.to = 0;
		if (rtsw.to == rtsw.from)
			++rtsw.to;

		expected = rtsw.from + i * 1000;
		if ((mode && param->fp & UFPP) || (!mode && param->fp & UFPS))
			fp_regs_set(expected);
		err = ioctl(fd, RTTST_RTIOC_SWTEST_SWITCH_TO, &rtsw);
		while (err == -1 && errno == EINTR)
			err = ioctl(fd, RTTST_RTIOC_SWTEST_PEND, &param->swt);

		switch (err) {
		case 0:
			break;
		case 1:
			handle_bad_fpreg(param->cpu, ~0);
		case -1:
			pthread_exit(NULL);
		}
		if ((mode && param->fp & UFPP) || (!mode && param->fp & UFPS)) {
			fp_val = fp_regs_check(expected);
			if (fp_val != expected)
				handle_bad_fpreg(param->cpu, fp_val);
		}

		/* Switch mode. */
		mode = PTHREAD_PRIMARY - mode;
		if ((err = pthread_set_mode_np(mode, PTHREAD_PRIMARY - mode))) {
			fprintf(stderr,
				"rtuo: pthread_set_mode_np: %s\n",
				strerror(err));
			exit(EXIT_FAILURE);
		}

		if(++i == 4000000)
			i = 0;
	}

	return NULL;
}

static int parse_arg(struct task_params *param,
		     const char *text,
		     struct cpu_tasks *cpus)
{
	struct t2f {
		const char *text;
		unsigned flag;
	};

	static struct t2f type2flags [] = {
		{ "rtk",  RTK  },
		{ "rtup", RTUP },
		{ "rtus", RTUS },
		{ "rtuo", RTUO }
	};

	static struct t2f fp2flags [] = {
		{ "_fp",   AFP	 },
		{ "_ufpp", UFPP },
		{ "_ufps", UFPS }
	};

	unsigned long cpu;
	char *cpu_end;
	unsigned i;

	param->type = param->fp = 0;
	param->cpu = &cpus[0];

	for(i = 0; i < sizeof(type2flags)/sizeof(struct t2f); i++) {
		size_t len = strlen(type2flags[i].text);

		if(!strncmp(text, type2flags[i].text, len)) {
			param->type = type2flags[i].flag;
			text += len;
			goto fpflags;
		}
	}

	return -1;

  fpflags:
	if (*text == '\0')
		return 0;

	if (isdigit(*text))
		goto cpu_nr;

	for(i = 0; i < sizeof(fp2flags)/sizeof(struct t2f); i++) {
		size_t len = strlen(fp2flags[i].text);
	
		if(!strncmp(text, fp2flags[i].text, len)) {
			param->fp |= fp2flags[i].flag;
			text += len;

			goto fpflags;
		}
	}

	return -1;

  cpu_nr:
	cpu = strtoul(text, &cpu_end, 0);

	if (*cpu_end != '\0' || ((cpu == 0 || cpu == ULONG_MAX) && errno))
		return -1;

	param->cpu = &cpus[cpu];
	return 0;
}

static int check_arg(const struct task_params *param, struct cpu_tasks *end_cpu)
{
	if (param->cpu > end_cpu - 1)
		return -1;

	switch (param->type) {
	case SLEEPER:
		if (param->fp)
			return -1;
		break;

	case RTK:
		if (param->fp & UFPS)
			return -1;
		break;

	case RTUP:
		if (param->fp & UFPS)
			return -1;
		break;

	case RTUS:
		if (param->fp & UFPP)
			return -1;
		break;

	case RTUO:
		break;
	default:
		return -1;
	}

	return 0;
}

static int task_create(struct cpu_tasks *cpu,
		       struct task_params *param,
		       pthread_attr_t *rt_attr,
		       pthread_attr_t *sleeper_attr)
{
	typedef void *thread_routine(void *);
	thread_routine *task_routine [] = {
		[RTUP] = &rtup,
		[RTUS] = &rtus,
		[RTUO] = &rtuo
	};
	int err;

	switch(param->type) {
	case RTK:
		param->swt.flags = (param->fp & AFP ? RTTST_SWTEST_FPU : 0)
			| (param->fp & UFPP ? RTTST_SWTEST_USE_FPU : 0);

		err=ioctl(cpu->fd,RTTST_RTIOC_SWTEST_CREATE_KTASK,&param->swt);
		if (err) {
			perror("ioctl(RTTST_RTIOC_SWTEST_CREATE_KTASK)");
			return -1;
		}
		break;

	case RTUP:
	case RTUS:
	case RTUO:
	case SLEEPER:
		param->swt.flags = 0;

		err=ioctl(cpu->fd,RTTST_RTIOC_SWTEST_REGISTER_UTASK,&param->swt);
		if (err) {
			perror("ioctl(RTTST_RTIOC_SWTEST_REGISTER_UTASK)");
			return -1;
		}
		break;

	default:
		fprintf(stderr, "Invalid task type %d. Aborting\n", param->type);
		return EINVAL;
	}

	if (param->type == RTK)
		return 0;

	if (param->type == SLEEPER) {
		err = __real_pthread_create(&param->thread,
					    sleeper_attr,
					    sleeper,
					    param);

		if (err)
			fprintf(stderr,"pthread_create: %s\n",strerror(err));

		return err;
	}

	err = pthread_create(&param->thread, rt_attr,
			     task_routine[param->type], param);
	if (err) {
		fprintf(stderr, "pthread_create: %s\n", strerror(err));
		return err;
	}

	err = pthread_set_name_np(param->thread,
				  task_name(param->cpu,param->swt.index));
			
	if (err)
		fprintf(stderr,"pthread_set_name_np: %s\n", strerror(err));

	return err;
}

#define DEV_NR_MAX 256

static int open_rttest(char *buf, size_t size, unsigned count)
{
	static unsigned dev_nr = 0;
	int fd, status;

	do {
		snprintf(buf, size, "/dev/rttest%u", dev_nr);

		status = fd = open(buf, O_RDWR);
		
		if (fd == -1)
			goto next_dev;
		
		status = ioctl(fd, RTTST_RTIOC_SWTEST_SET_TASKS_COUNT, count);

		if (status == 0)
			break;
		
	  next_dev:
		if (fd != -1)
			close(fd);

		if (++dev_nr != DEV_NR_MAX)
			continue;

		fprintf(stderr, "switchtest: Unable to open switchtest device.\n"
			"(modprobe xeno_switchtest ?)\n");

		return -1;
	} while (status == -1);

	return fd;
}

const char *all_nofp [] = {
	"rtk",
	"rtk",
	"rtup",
	"rtup",
	"rtus",
	"rtus",
	"rtuo",
	"rtuo",
};

const char *all_fp [] = {
	"rtk",
	"rtk",
	"rtk_fp",
	"rtk_fp",
	"rtk_fp_ufpp",
	"rtk_fp_ufpp",
	"rtup",
	"rtup",
	"rtup_ufpp",
	"rtup_ufpp",
	"rtus",
	"rtus",
	"rtus_ufps",
	"rtus_ufps",
	"rtuo",
	"rtuo",
	"rtuo_ufpp",
	"rtuo_ufpp",
	"rtuo_ufps",
	"rtuo_ufps",
	"rtuo_ufpp_ufps",
	"rtuo_ufpp_ufps"
};

void usage(FILE *fd, const char *progname)
{
	unsigned i, j, nr_cpus;

	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	fprintf(fd,
		"Usage:\n"
		"%s threadspec threadspec...\n"
		"or %s [-n]\n\n"
		"Where threadspec specifies the characteristics of a thread to"
		" be created:\n"
		"threadspec = (rtk|rtup|rtus|rtuo)(_fp|_ufpp|_ufps)*[0-9]*\n"
		"rtk for a kernel-space real-time thread;\n"
		"rtup for a user-space real-time thread running in primary"
		" mode,\n"
		"rtus for a user-space real-time thread running in secondary"
		" mode,\n"
		"rtuo for a user-space real-time thread oscillating between"
		" primary and\n	    secondary mode,\n\n"
		"_fp means that the created thread will have the XNFPU bit"
		" armed (only valid for\n     rtk),\n"
		"_ufpp means that the created thread will use the FPU when in "
		"primary mode\n	    (invalid for rtus),\n"
		"_ufps means that the created thread will use the FPU when in "
		"secondary mode\n     (invalid for rtk and rtup),\n\n"
		"[0-9]* specifies the ID of the CPU where the created thread "
		"will run, 0 if\n	unspecified.\n\n"
		"Passing no argument is equivalent to running:\n%s",
		progname, progname, progname);

	for (i = 0; i < nr_cpus; i++)
		for (j = 0; j < sizeof(all_fp)/sizeof(char *); j++)
			fprintf(fd, " %s%d", all_fp[j], i);

	fprintf(fd,
		"\n\nPassing only the -n argument is equivalent to running:\n%s",
		progname);

	for (i = 0; i < nr_cpus; i++)
		for (j = 0; j < sizeof(all_nofp)/sizeof(char *); j++)
			fprintf(fd, " %s%d", all_nofp[j], i);
	fprintf(fd, "\n\n");
}

int main(int argc, const char *argv[])
{
	const char **all, *progname = argv[0];
	pthread_attr_t rt_attr, sleeper_attr;
	unsigned i, j, count, nr_cpus;
	struct cpu_tasks *cpus;
	struct sched_param sp;
	char devname[21];
	int status, sig;
	sigset_t mask;

	/* Initializations. */
	if (mlockall(MCL_CURRENT|MCL_FUTURE)) {
		perror("mlockall");
		exit(EXIT_FAILURE);
	}

	if (__real_sem_init(&sleeper_start, 0, 0)) {
		perror("sem_init");
		exit(EXIT_FAILURE);
	}

	all = all_fp;
	count = sizeof(all_fp) / sizeof(char *);
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (nr_cpus == -1) {
		fprintf(stderr,
			"Error %d while getting the number of cpus (%s)\n",
			errno,
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Check for -n, -h or --help flag. */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n")) {
			if (argc != 2) {
				usage(stderr, progname);
				fprintf(stderr,
					"-n option may only be used with no "
					"other argument.\n");
				exit(EXIT_FAILURE);
			}

			all = all_nofp;
			count = sizeof(all_nofp) / sizeof(char *);
			--argc;
		}

		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout, progname);
			exit(EXIT_SUCCESS);
		}
	}

	if (setvbuf(stdout, NULL, _IOLBF, 0)) {
		perror("setvbuf");
		exit(EXIT_FAILURE);
	}

	/* If no argument was passed (or only -n), replace argc and argv with
	   default values, given by all_fp or all_nofp depending on the presence
	   of the -n flag. */
	if (argc == 1) {
		char buffer[32];

		/* Check if fp routines are dummy. */
		if (all == all_fp) {
			fprintf(stderr, "Testing FPU check routines...\n");
			fp_regs_set(1);
			if (fp_regs_check(2) != 1) {
				fprintf(stderr,
					"FPU check routines unimplemented, "
					"skipping FPU switches tests.\n");
				all = all_nofp;
				count = sizeof(all_nofp) / sizeof(char *);
			} else
				fprintf(stderr, "FPU check routines OK.\n");
		}

		argc = count * nr_cpus + 1;
		argv = (const char **) malloc(argc * sizeof(char *));
		argv[0] = progname;
		for (i = 0; i < nr_cpus; i++)
			for (j = 0; j < count; j++) {
				snprintf(buffer,
					 sizeof(buffer),
					 "%s%d",
					 all[j],
					 i);
				argv[i * count + j + 1] = strdup(buffer);
			}
	}

	cpus = (struct cpu_tasks *) malloc(sizeof(*cpus) * nr_cpus);
	if (!cpus) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < nr_cpus; i++) {
		size_t size;
		cpus[i].fd = -1;
		cpus[i].index = i;
		cpus[i].capacity = 2;
		size = cpus[i].capacity * sizeof(struct task_params);
		cpus[i].tasks_count = 1;
		cpus[i].tasks = (struct task_params *) malloc(size);

		if (!cpus[i].tasks) {
			perror("malloc");
			exit(EXIT_FAILURE);
		}

		cpus[i].tasks[0].type = SLEEPER;
		cpus[i].tasks[0].fp = 0;
		cpus[i].tasks[0].cpu = &cpus[i];	
		cpus[i].tasks[0].thread = 0;
		cpus[i].tasks[0].swt.index = cpus[i].tasks[0].swt.flags = 0;
	}

	/* Parse arguments and build data structures. */
	for(i = 1; i < argc; i++) {
		struct task_params params;
		struct cpu_tasks *cpu;

		if(parse_arg(&params, argv[i], cpus)) {
			usage(stderr, progname);
			fprintf(stderr,
				"Unable to parse %s. Aborting.\n",
				argv[i]);
			exit(EXIT_FAILURE);
		}

		if (check_arg(&params, &cpus[nr_cpus])) {
			usage(stderr, progname);
			fprintf(stderr,
				"Invalid parameters %s. Aborting\n",
				argv[i]);
			exit(EXIT_FAILURE);
		}

		cpu = params.cpu;
		if(++cpu->tasks_count > cpu->capacity) {
			size_t size;
			cpu->capacity += cpu->capacity / 2;
			size = cpu->capacity * sizeof(struct task_params);
			cpu->tasks =
				(struct task_params *) realloc(cpu->tasks, size);
			if (!cpu->tasks) {
				perror("realloc");
				exit(EXIT_FAILURE);
			}
		}

		params.thread = 0;
		params.swt.index = params.swt.flags = 0;
		cpu->tasks[cpu->tasks_count - 1] = params;
	}

	/* For best compatibility with both LinuxThreads and NPTL, block the
	   termination signals on all threads. */
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);
	
	/* Prepare attributes for real-time tasks. */
	pthread_attr_init(&rt_attr);
	pthread_attr_setinheritsched(&rt_attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&rt_attr, SCHED_FIFO);
	sp.sched_priority = 1;
	pthread_attr_setschedparam(&rt_attr, &sp);
	pthread_attr_setstacksize(&rt_attr, 20 * 1024);

	/* Prepare attribute for sleeper tasks. */
	pthread_attr_init(&sleeper_attr);
	pthread_attr_setstacksize(&sleeper_attr, 20 * 1024);

	/* Create and register all tasks. */
	for (i = 0; i < nr_cpus; i ++) {
		struct cpu_tasks *cpu = &cpus[i];

		cpu->fd = open_rttest(devname,sizeof(devname),cpu->tasks_count);

		if (cpu->fd == -1)
			goto failure;

		if (ioctl(cpu->fd, RTTST_RTIOC_SWTEST_SET_CPU, i)) {
			perror("ioctl(RTTST_RTIOC_SWTEST_SET_CPU)");
			goto failure;
		}

		for (j = 0; j < cpu->tasks_count; j++) {
			struct task_params *param = &cpu->tasks[j];
			if (task_create(cpu, param, &rt_attr, &sleeper_attr)) {
			  failure:
				status = EXIT_FAILURE;
				goto cleanup;
			}
		}
	}

	/* Start the sleeper tasks. */
	for (i = 0; i < nr_cpus; i ++)
		__real_sem_post(&sleeper_start);

	/* Wait for interruption. */
	sigwait(&mask, &sig);

	/* Allow a second Ctrl-C in case of lockup. */
	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	status = EXIT_SUCCESS;

	/* Cleanup. */
  cleanup:
	for (i = 0; i < nr_cpus; i ++) {
		struct cpu_tasks *cpu = &cpus[i];

		/* kill the user-space tasks. */
		for (j = 0; j < cpu->tasks_count; j++) {
			struct task_params *param = &cpu->tasks[j];

			if (param->type != RTK && param->thread)
				pthread_cancel(param->thread);
		}

		/* join them. */
		for (j = 0; j < cpu->tasks_count; j++) {
			struct task_params *param = &cpu->tasks[j];

			if (param->type != RTK && param->thread)
				pthread_join(param->thread, NULL);
		}

		/* Kill the kernel-space tasks. */
		if (cpus[i].fd != -1)
			close(cpus[i].fd);
		free(cpu->tasks);
	}
	free(cpus);
	__real_sem_destroy(&sleeper_start);

	return status;
}
