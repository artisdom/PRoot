/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot: a PTrace based chroot alike.
 *
 * Copyright (C) 2010 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 *
 * Author: Cedric VINCENT (cedric.vincent@st.com)
 * Inspired by: QEMU user-mode.
 */

#define _GNU_SOURCE      /* O_NOFOLLOW in fcntl.h, */
#include <fcntl.h>       /* AT_FDCWD, */
#include <sys/types.h>   /* pid_t, */
#include <sys/inotify.h> /* IN_DONT_FOLLOW, */
#include <assert.h>      /* assert(3), */
#include <limits.h>      /* PATH_MAX, */
#include <stddef.h>      /* intptr_t, */
#include <errno.h>       /* errno(3), */
#include <stdio.h>       /* fprintf(3), */
#include <sys/ptrace.h>  /* ptrace(2), PTRACE_*, */
#include <sys/user.h>    /* struct user*, */
#include <stdlib.h>      /* exit(3), */
#include <string.h>      /* strlen(3), */

#include "syscall.h"
#include "arch.h"  /* word_t, SYSCALL_AVOIDER, __NR_*, */
#include "child.h" /* copy_*_child(), get_child_string(), */
#include "path.h"  /* [de]translate_path(), */

/**
 * Specify the offset in the child's USER area of each register used
 * for syscall argument passing. */
size_t arg_offset[] = {
	USER_REGS_OFFSET(REG_SYSARG_NUM),
	USER_REGS_OFFSET(REG_SYSARG_1),
	USER_REGS_OFFSET(REG_SYSARG_2),
	USER_REGS_OFFSET(REG_SYSARG_3),
	USER_REGS_OFFSET(REG_SYSARG_4),
	USER_REGS_OFFSET(REG_SYSARG_5),
	USER_REGS_OFFSET(REG_SYSARG_6),
	USER_REGS_OFFSET(REG_SYSARG_RESULT),
};

/**
 * Return the @sysarg argument of the current syscall in the
 * child process @pid.
 */
word_t get_sysarg(pid_t pid, enum sysarg sysarg)
{
	word_t result;

	/* Sanity checks. */
	assert(sysarg >= SYSARG_FIRST);
	assert(sysarg <= SYSARG_LAST);

	/* Get the argument register from the child's USER area. */
	result = ptrace(PTRACE_PEEKUSER, pid, arg_offset[sysarg], NULL);
	if (errno != 0) {
		perror("proot -- ptrace(PEEKUSER)");
		exit(EXIT_FAILURE);
	}

	return result;
}

/**
 * Set the @sysarg argument of the current syscall in the child
 * process @pid to @value.
 */
static void set_sysarg(pid_t pid, enum sysarg sysarg, word_t value)
{
	long status;

	/* Sanity checks. */
	assert(sysarg >= SYSARG_FIRST);
	assert(sysarg <= SYSARG_LAST);

	/* Set the argument register in the child's USER area. */
	status = ptrace(PTRACE_POKEUSER, pid, arg_offset[sysarg], value);
	if (status < 0) {
		perror("proot -- ptrace(POKEUSER)");
		exit(EXIT_FAILURE);
	}
}

/**
 * Copie in @path a C string (PATH_MAX bytes max.) from the @pid
 * child's memory address space pointed to by the @sysarg argument of
 * the current syscall.
 */
static inline int get_sysarg_path(pid_t pid, char path[PATH_MAX], enum sysarg sysarg)
{
	int size;
	
	size = get_child_string(pid, path, get_sysarg(pid, sysarg), PATH_MAX);
	if (size < 0)
		return size;
	if (size >= PATH_MAX)
		return -ENAMETOOLONG;

	path[size] = '\0';
	return size;
}

/**
 * Translate @path and puts the result in the @pid child's memory
 * address space pointed to by the @sysarg argument of the current
 * syscall. See the documentation of translate_path() about the
 * meaning of @deref_final. This function returns -errno if an error
 * occured, otherwise it returns the size in bytes "allocated" into
 * the stack.
 */
static int translate_path2sysarg(pid_t pid, char path[PATH_MAX], enum sysarg sysarg, int deref_final)
{
	char new_path[PATH_MAX];
	word_t child_ptr;
	int status;
	int size;

	/* Translate the original path. */
	status = translate_path(pid, new_path, path, deref_final);
	if (status < 0)
		return status;

	/* Allocate space into the child's stack to host the new path. */
	size = strlen(new_path) + 1;
	child_ptr = resize_child_stack(pid, size);
	if (child_ptr == 0)
		return -EFAULT;

	/* Copy the new path into the previously allocated space. */
	status = copy_to_child(pid, child_ptr, new_path, size);
	if (status < 0) {
		(void) resize_child_stack(pid, -size);
		return status;
	}

	/* Make this argument point to the new path. */
	set_sysarg(pid, sysarg, child_ptr);

	return size;
}

/**
 * A helper, see the comment of the function above.
 */
static int translate_sysarg(pid_t pid, enum sysarg sysarg, int deref_final)
{
	char old_path[PATH_MAX];
	int status;

	/* Extract the original path. */
	status = get_sysarg_path(pid, old_path, sysarg);
	if (status < 0)
		return status;

	return translate_path2sysarg(pid, old_path, sysarg, deref_final);
}

/**
 * Detranslate the current @sysarg syscall argument of the child
 * @pid. If @size if greater or equal to 0, the argument is considered
 * as a buffer of @size bytes, that is, the C string terminator is
 * ignored. It returns the number of bytes used by the path pointed to
 * by @sysarg, including the string terminator.
 */
static int detranslate_sysarg(pid_t pid, enum sysarg sysarg, int size, int weakness)
{
	char path[PATH_MAX];
	word_t child_ptr;
	int status;

	assert(size >= 0);

	/* Extract the original path, be careful since some syscalls
	 * like readlink*(2) do not put a C string terminator. */
	if (size >= PATH_MAX)
		return -ENAMETOOLONG;

	status = copy_from_child(pid, path, get_sysarg(pid, sysarg), size);
	if (status < 0)
		return status;
	path[size] = '\0';

	/* Removes the leading "root" part. */
	status = detranslate_path(path, weakness);
	if (status < 0)
		return status;

	/* The original path doesn't need detranslation. */
	if (status == 0)
		goto skip_overwrite;

	/* Overwrite the path. */
	child_ptr = get_sysarg(pid, sysarg);
	status = copy_to_child(pid, child_ptr, path, status);
	if (status < 0)
		return status;

skip_overwrite:
	return strlen(path) + 1;
}

/* Helper macros. */
#define REGULAR 1
#define SYMLINK 0

#define STRONG  1
#define WEAK    0

/**
 * Translate the input arguments of the syscall @sysnum in the @pid
 * child's process area. It returns -errno if an error occured,
 * otherwise this function returns the amount of bytes "allocated" in
 * the child's stack for storing the newly translated paths.
 *
 * For each of the following cases, I didn't write any comment since
 * the corresponding man-page really is the documentation.
 */
int translate_syscall_enter(pid_t pid, word_t sysnum)
{
	int flags;
	int dirfd;
	int olddirfd;
	int newdirfd;

	int status;
	int status1;
	int status2;

	char path[PATH_MAX];
	char oldpath[PATH_MAX];
	char newpath[PATH_MAX];

	/* Ensure one is not trying to cheat PRoot by calling an
	 * unsupported syscall on that architecture. */
	if ((int)sysnum < 0) {
		fprintf(stderr, "proot: forbidden syscall %lu\n", sysnum);
		return -ENOSYS;
	}

	/* Translate input arguments. */
	switch (sysnum) {
	case __NR__llseek:
	case __NR__newselect:
	case __NR__sysctl:
	case __NR_accept:
	case __NR_accept4:
	case __NR_add_key:
	case __NR_adjtimex:
	case __NR_afs_syscall:
	case __NR_alarm:
	case __NR_arch_prctl:
	case __NR_bdflush:
	case __NR_bind:
	case __NR_break:
	case __NR_brk:
	case __NR_capget:
	case __NR_capset:
	case __NR_clock_getres:
	case __NR_clock_gettime:
	case __NR_clock_nanosleep:
	case __NR_clock_settime:
	case __NR_clone:
	case __NR_close:
	case __NR_connect:
	case __NR_create_module:
	case __NR_delete_module:
	case __NR_dup:
	case __NR_dup2:
	case __NR_dup3:
	case __NR_epoll_create:
	case __NR_epoll_create1:
	case __NR_epoll_ctl:
	case __NR_epoll_ctl_old:
	case __NR_epoll_pwait:
	case __NR_epoll_wait:
	case __NR_epoll_wait_old:
	case __NR_eventfd:
	case __NR_eventfd2:
	case __NR_exit:
	case __NR_exit_group:
	case __NR_fadvise64:
	case __NR_fadvise64_64:
	case __NR_fallocate:
	case __NR_fchdir:
	case __NR_fchmod:
	case __NR_fchown:
	case __NR_fchown32:
	case __NR_fcntl:
	case __NR_fcntl64:
	case __NR_fdatasync:
	case __NR_fgetxattr:
	case __NR_flistxattr:
	case __NR_flock:
	case __NR_fork:
	case __NR_fremovexattr:
	case __NR_fsetxattr:
	case __NR_fstat:
	case __NR_fstat64:
	case __NR_fstatfs:
	case __NR_fstatfs64:
	case __NR_fsync:
	case __NR_ftime:
	case __NR_ftruncate:
	case __NR_ftruncate64:
	case __NR_futex:
	case __NR_get_kernel_syms:
	case __NR_get_mempolicy:
	case __NR_get_robust_list:
	case __NR_get_thread_area:
	case __NR_getcpu:
	case __NR_getdents:
	case __NR_getdents64:
	case __NR_getegid:
	case __NR_getegid32:
	case __NR_geteuid:
	case __NR_geteuid32:
	case __NR_getgid:
	case __NR_getgid32:
	case __NR_getgroups:
	case __NR_getgroups32:
	case __NR_getitimer:
	case __NR_getpeername:
	case __NR_getpgid:
	case __NR_getpgrp:
	case __NR_getpid:
	case __NR_getpmsg:
	case __NR_getppid:
	case __NR_getpriority:
	case __NR_getresgid:
	case __NR_getresgid32:
	case __NR_getresuid:
	case __NR_getresuid32:
	case __NR_getrlimit:
	case __NR_getrusage:
	case __NR_getsid:
	case __NR_getsockname:
	case __NR_getsockopt:
	case __NR_gettid:
	case __NR_gettimeofday:
	case __NR_getuid:
	case __NR_getuid32:
	case __NR_gtty:
	case __NR_idle:
	case __NR_init_module:
	case __NR_inotify_init:
	case __NR_inotify_init1:
	case __NR_inotify_rm_watch:
	case __NR_io_cancel:
	case __NR_io_destroy:
	case __NR_io_getevents:
	case __NR_io_setup:
	case __NR_io_submit:
	case __NR_ioctl:
	case __NR_ioperm:
	case __NR_iopl:
	case __NR_ioprio_get:
	case __NR_ioprio_set:
	case __NR_ipc:
	case __NR_kexec_load:
	case __NR_keyctl:
	case __NR_kill:
	case __NR_listen:
	case __NR_lock:
	case __NR_lookup_dcookie:
	case __NR_lseek:
	case __NR_madvise:
	case __NR_madvise1:
	case __NR_mbind:
	case __NR_migrate_pages:
	case __NR_mincore:
	case __NR_mlock:
	case __NR_mlockall:
	case __NR_mmap:
	case __NR_mmap2:
	case __NR_modify_ldt:
	case __NR_move_pages:
	case __NR_mprotect:
	case __NR_mpx:
	case __NR_mq_getsetattr:
	case __NR_mq_notify:
	case __NR_mq_open:
	case __NR_mq_timedreceive:
	case __NR_mq_timedsend:
	case __NR_mq_unlink:
	case __NR_mremap:
	case __NR_msgctl:
	case __NR_msgget:
	case __NR_msgrcv:
	case __NR_msgsnd:
	case __NR_msync:
	case __NR_munlock:
	case __NR_munlockall:
	case __NR_munmap:
	case __NR_nanosleep:
	case __NR_nfsservctl:
	case __NR_nice:
	case __NR_oldfstat:
	case __NR_oldolduname:
	case __NR_olduname:
	case __NR_pause:
	case __NR_perf_event_open:
	case __NR_personality:
	case __NR_pipe:
	case __NR_pipe2:
	case __NR_poll:
	case __NR_ppoll:
	case __NR_prctl:
	case __NR_pread64:
	case __NR_preadv:
	case __NR_prof:
	case __NR_profil:
	case __NR_pselect6:
	case __NR_ptrace: /* Here comes the exit door ;) Do NOT use PRoot for security purpose! */
	case __NR_putpmsg:
	case __NR_pwrite64:
	case __NR_pwritev:
	case __NR_query_module:
	case __NR_quotactl:
	case __NR_read:
	case __NR_readahead:
	case __NR_readdir:
	case __NR_readv:
	case __NR_reboot:
	case __NR_recvfrom:
	case __NR_recvmmsg:
	case __NR_recvmsg:
	case __NR_remap_file_pages:
	case __NR_request_key:
	case __NR_restart_syscall:
	case __NR_rt_sigaction:
	case __NR_rt_sigpending:
	case __NR_rt_sigprocmask:
	case __NR_rt_sigqueueinfo:
	case __NR_rt_sigreturn:
	case __NR_rt_sigsuspend:
	case __NR_rt_sigtimedwait:
	case __NR_rt_tgsigqueueinfo:
	case __NR_sched_get_priority_max:
	case __NR_sched_get_priority_min:
	case __NR_sched_getaffinity:
	case __NR_sched_getparam:
	case __NR_sched_getscheduler:
	case __NR_sched_rr_get_interval:
	case __NR_sched_setaffinity:
	case __NR_sched_setparam:
	case __NR_sched_setscheduler:
	case __NR_sched_yield:
	case __NR_security:
	case __NR_select:
	case __NR_semctl:
	case __NR_semget:
	case __NR_semop:
	case __NR_semtimedop:
	case __NR_sendfile:
	case __NR_sendfile64:
	case __NR_sendmsg:
	case __NR_sendto:
	case __NR_set_mempolicy:
	case __NR_set_robust_list:
	case __NR_set_thread_area:
	case __NR_set_tid_address:
	case __NR_setdomainname:
	case __NR_setfsgid:
	case __NR_setfsgid32:
	case __NR_setfsuid:
	case __NR_setfsuid32:
	case __NR_setgid:
	case __NR_setgid32:
	case __NR_setgroups:
	case __NR_setgroups32:
	case __NR_sethostname:
	case __NR_setitimer:
	case __NR_setpgid:
	case __NR_setpriority:
	case __NR_setregid:
	case __NR_setregid32:
	case __NR_setresgid:
	case __NR_setresgid32:
	case __NR_setresuid:
	case __NR_setresuid32:
	case __NR_setreuid:
	case __NR_setreuid32:
	case __NR_setrlimit:
	case __NR_setsid:
	case __NR_setsockopt:
	case __NR_settimeofday:
	case __NR_setuid:
	case __NR_setuid32:
	case __NR_sgetmask:
	case __NR_shmat:
	case __NR_shmctl:
	case __NR_shmdt:
	case __NR_shmget:
	case __NR_shutdown:
	case __NR_sigaction:
	case __NR_sigaltstack:
	case __NR_signal:
	case __NR_signalfd:
	case __NR_signalfd4:
	case __NR_sigpending:
	case __NR_sigprocmask:
	case __NR_sigreturn:
	case __NR_sigsuspend:
	case __NR_socket:
	case __NR_socketcall:
	case __NR_socketpair:
	case __NR_splice:
	case __NR_ssetmask:
	case __NR_stime:
	case __NR_stty:
	case __NR_sync:
	case __NR_sync_file_range:
	case __NR_sysfs:
	case __NR_sysinfo:
	case __NR_syslog:
	case __NR_tee:
	case __NR_tgkill:
	case __NR_time:
	case __NR_timer_create:
	case __NR_timer_delete:
	case __NR_timer_getoverrun:
	case __NR_timer_gettime:
	case __NR_timer_settime:
	case __NR_timerfd_create:
	case __NR_timerfd_gettime:
	case __NR_timerfd_settime:
	case __NR_times:
	case __NR_tkill:
	case __NR_tuxcall:
	case __NR_ugetrlimit:
	case __NR_ulimit:
	case __NR_umask:
	case __NR_uname:
	case __NR_unshare:
	case __NR_ustat:
	case __NR_vfork:
	case __NR_vhangup:
	case __NR_vm86:
	case __NR_vm86old:
	case __NR_vmsplice:
	case __NR_vserver:
	case __NR_wait4:
	case __NR_waitid:
	case __NR_waitpid:
	case __NR_write:
	case __NR_writev:
	case __NR_getcwd:

#ifdef arm
	case __ARM_NR_breakpoint:
	case __ARM_NR_cacheflush:
	case __ARM_NR_set_tls:
	case __ARM_NR_usr26:
	case __ARM_NR_usr32:
#endif /* arm */

		/* Nothing to do. */
		status = 0;
		break;

	case __NR_access:
	case __NR_acct:
	case __NR_chdir:
	case __NR_chmod:
	case __NR_chown:
	case __NR_chown32:
	case __NR_chroot:
	case __NR_execve:
	case __NR_getxattr:
	case __NR_listxattr:
	case __NR_mkdir:
	case __NR_mknod:
	case __NR_oldstat:
	case __NR_creat:
	case __NR_removexattr:
	case __NR_rmdir:
	case __NR_setxattr:
	case __NR_stat:
	case __NR_stat64:
	case __NR_statfs:
	case __NR_statfs64:
	case __NR_swapoff:
	case __NR_swapon:
	case __NR_truncate:
	case __NR_truncate64:
	case __NR_unlink:
	case __NR_uselib:
	case __NR_utime:
	case __NR_utimes:
		status = translate_sysarg(pid, SYSARG_1, REGULAR);
		break;

	case __NR_open:
		flags = get_sysarg(pid, SYSARG_2);

		if (   ((flags & O_NOFOLLOW) != 0)
		    || ((flags & O_EXCL) != 0 && (flags & O_CREAT) != 0))
			status = translate_sysarg(pid, SYSARG_1, SYMLINK);
		else
			status = translate_sysarg(pid, SYSARG_1, REGULAR);
		break;

	case __NR_faccessat:
	case __NR_fchmodat:
	case __NR_fchownat:
	case __NR_fstatat64:
	case __NR_newfstatat:
	case __NR_utimensat:
		dirfd = get_sysarg(pid, SYSARG_1);
		status = get_sysarg_path(pid, path, SYSARG_2);
		if (status < 0)
			break;

		flags = (sysnum == __NR_fchownat)
			? get_sysarg(pid, SYSARG_5)
			: get_sysarg(pid, SYSARG_4);

		if (!AT_FD(dirfd, path)) {
			if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
				status = translate_path2sysarg(pid, path, SYSARG_2, SYMLINK);
			else
				status = translate_path2sysarg(pid, path, SYSARG_2, REGULAR);
		}
		else {
			if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
				status = check_path_at(pid, dirfd, path, SYMLINK);
			else
				status = check_path_at(pid, dirfd, path, REGULAR);
		}
		break;

	case __NR_futimesat:
	case __NR_mkdirat:
	case __NR_mknodat:
	case __NR_unlinkat:
		dirfd = get_sysarg(pid, SYSARG_1);
		status = get_sysarg_path(pid, path, SYSARG_2);
		if (status < 0)
			break;

		if (!AT_FD(dirfd, path))
			status = translate_path2sysarg(pid, path, SYSARG_2, REGULAR);
		else
			status = check_path_at(pid, dirfd, path, REGULAR);
		break;

	case __NR_inotify_add_watch:
		flags = get_sysarg(pid, SYSARG_3);

		if ((flags & IN_DONT_FOLLOW) != 0)
			status = translate_sysarg(pid, SYSARG_2, SYMLINK);
		else
			status = translate_sysarg(pid, SYSARG_2, REGULAR);
		break;

	case __NR_lchown:
	case __NR_lchown32:
	case __NR_lgetxattr:
	case __NR_llistxattr:
	case __NR_lremovexattr:
	case __NR_lsetxattr:
	case __NR_lstat:
	case __NR_lstat64:
	case __NR_oldlstat:
		status = translate_sysarg(pid, SYSARG_1, SYMLINK);
		break;

	case __NR_link:
	case __NR_pivot_root:
		status1 = translate_sysarg(pid, SYSARG_1, REGULAR);
		status2 = translate_sysarg(pid, SYSARG_2, REGULAR);
		if (status1 < 0) {
			status = status1;
			break;
		}
		if (status2 < 0) {
			status = status2;
			break;
		}
		status = status1 + status2;
		break;

	case __NR_linkat:
		olddirfd = get_sysarg(pid, SYSARG_1);
		newdirfd = get_sysarg(pid, SYSARG_3);
		flags    = get_sysarg(pid, SYSARG_5);

		status1 = get_sysarg_path(pid, oldpath, SYSARG_2);
		status2 = get_sysarg_path(pid, newpath, SYSARG_4);
		if (status1 < 0) {
			status = status1;
			break;
		}
		if (status2 < 0) {
			status = status2;
			break;
		}

		if (!AT_FD(olddirfd, oldpath) && !AT_FD(newdirfd, newpath)) {
			if ((flags & AT_SYMLINK_FOLLOW) != 0) {
				status1 = translate_path2sysarg(pid, oldpath, SYSARG_2, REGULAR);
				status2 = translate_path2sysarg(pid, newpath, SYSARG_4, REGULAR);
			} else {
				status1 = translate_path2sysarg(pid, oldpath, SYSARG_2, SYMLINK);
				status2 = translate_path2sysarg(pid, newpath, SYSARG_4, REGULAR);
			}
		}
		else if (!AT_FD(olddirfd, oldpath) && AT_FD(newdirfd, newpath)) {
			if ((flags & AT_SYMLINK_FOLLOW) != 0)
				status1 = translate_path2sysarg(pid, oldpath, SYSARG_2, REGULAR);
			else
				status1 = translate_path2sysarg(pid, oldpath, SYSARG_2, SYMLINK);

			status2 = check_path_at(pid, newdirfd, newpath, REGULAR);
		}
		else if (AT_FD(olddirfd, oldpath) && !AT_FD(newdirfd, newpath)) {
			if ((flags & AT_SYMLINK_FOLLOW) != 0)
				status1 = check_path_at(pid, olddirfd, oldpath, REGULAR);
			else
				status1 = check_path_at(pid, olddirfd, oldpath, SYMLINK);

			status2 = translate_path2sysarg(pid, newpath, SYSARG_4, REGULAR);
		}
		else {
			if ((flags & AT_SYMLINK_FOLLOW) != 0)
				status1 = check_path_at(pid, olddirfd, oldpath, REGULAR);
			else
				status1 = check_path_at(pid, olddirfd, oldpath, SYMLINK);

			status2 = check_path_at(pid, newdirfd, newpath, REGULAR);
		}

		if (status1 < 0) {
			status = status1;
			break;
		}
		if (status2 < 0) {
			status = status2;
			break;
		}
		status = status1 + status2;
		break;

	case __NR_mount:
	case __NR_umount:
	case __NR_umount2:
		status = get_sysarg_path(pid, path, SYSARG_1);
		if (status < 0)
			break;

		if (path[0] == '/' || path[0] == '.')
			status = translate_path2sysarg(pid, path, SYSARG_2, REGULAR);
		else
			status = 0;
		break;

	case __NR_openat:
		dirfd = get_sysarg(pid, SYSARG_1);
		flags = get_sysarg(pid, SYSARG_3);

		status = get_sysarg_path(pid, path, SYSARG_2);
		if (status < 0)
			break;

		if (!AT_FD(dirfd, path)) {
			if (   ((flags & O_NOFOLLOW) != 0)
			    || ((flags & O_EXCL) != 0 && (flags & O_CREAT) != 0))
				status = translate_path2sysarg(pid, path, SYSARG_2, SYMLINK);
			else
				status = translate_path2sysarg(pid, path, SYSARG_2, REGULAR);
		}
		else {
			if (   ((flags & O_NOFOLLOW) != 0)
			    || ((flags & O_EXCL) != 0 && (flags & O_CREAT) != 0))
				status = check_path_at(pid, dirfd, path, SYMLINK);
			else
				status = check_path_at(pid, dirfd, path, REGULAR);
		}
		break;

	case __NR_readlink:
		status = translate_sysarg(pid, SYSARG_1, SYMLINK);
		break;

	case __NR_readlinkat:
		dirfd = get_sysarg(pid, SYSARG_1);

		status = get_sysarg_path(pid, path, SYSARG_2);
		if (status < 0)
			break;

		if (!AT_FD(dirfd, path))
			status = translate_path2sysarg(pid, path, SYSARG_1, SYMLINK);
		else
			status = check_path_at(pid, dirfd, path, SYMLINK);
		break;

	case __NR_rename:
		status1 = translate_sysarg(pid, SYSARG_1, SYMLINK);
		status2 = translate_sysarg(pid, SYSARG_2, SYMLINK);

		if (status1 < 0) {
			status = status1;
			break;
		}
		if (status2 < 0) {
			status = status2;
			break;
		}
		status = status1 + status2;

		break;

	case __NR_renameat:
		olddirfd = get_sysarg(pid, SYSARG_1);
		newdirfd = get_sysarg(pid, SYSARG_3);

		status1 = get_sysarg_path(pid, oldpath, SYSARG_2);
		status2 = get_sysarg_path(pid, newpath, SYSARG_4);
		if (status1 < 0) {
			status = status1;
			break;
		}
		if (status2 < 0) {
			status = status2;
			break;
		}

		if (!AT_FD(olddirfd, oldpath) && !AT_FD(newdirfd, newpath)) {
			status1 = translate_path2sysarg(pid, oldpath, SYSARG_2, SYMLINK);
			status2 = translate_path2sysarg(pid, newpath, SYSARG_4, SYMLINK);
		}
		else if (!AT_FD(olddirfd, oldpath) && AT_FD(newdirfd, newpath)) {
			status1 = translate_path2sysarg(pid, oldpath, SYSARG_2, SYMLINK);
			status2 = check_path_at(pid, newdirfd, newpath, SYMLINK);
		}
		else if (AT_FD(olddirfd, oldpath) && !AT_FD(newdirfd, newpath)) {
			status1 = check_path_at(pid, olddirfd, oldpath, SYMLINK);
			status2 = translate_path2sysarg(pid, newpath, SYSARG_4, SYMLINK);
		}
		else {
			status1 = check_path_at(pid, olddirfd, oldpath, SYMLINK);
			status2 = check_path_at(pid, newdirfd, newpath, SYMLINK);
		}

		if (status1 < 0) {
			status = status1;
			break;
		}
		if (status2 < 0) {
			status = status2;
			break;
		}
		status = status1 + status2;

		break;

	case __NR_symlink:
		status = translate_sysarg(pid, SYSARG_4, REGULAR);
		break;

	case __NR_symlinkat:
		newdirfd = get_sysarg(pid, SYSARG_2);

		status = get_sysarg_path(pid, newpath, SYSARG_3);
		if (status < 0)
			break;

		if (!AT_FD(newdirfd, newpath))
			status = translate_path2sysarg(pid, newpath, SYSARG_2, REGULAR);
		else
			status = check_path_at(pid, newdirfd, newpath, REGULAR);
		break;

	default:
		fprintf(stderr, "proot: unknown syscall %lu\n", sysnum);
		status = -ENOSYS;
		break;
	}

	/* Avoid the actual syscall if an error occured during the
	 * translation. */
	if (status < 0)
		set_sysarg(pid, SYSARG_NUM, SYSCALL_AVOIDER);

	return status;
}

/**
 * Translate the output arguments of the syscall @sysnum in the @pid
 * child's process area. Is also sets the result of this syscall to
 * @status if an error occured previously during the translation, that
 * is, if @status is lesser than 0. Otherwise, @status bytes of the
 * child's stack are "deallocated" to free the space used to store the
 * previously translated paths.
 */
void translate_syscall_exit(pid_t pid, word_t sysnum, int status)
{
	word_t size;

	/* Set the child's errno if an error occured previously during
	 * the translation. */
	if (status < 0)
		set_sysarg(pid, SYSARG_RESULT, (word_t)status);
	
	/* De-allocate the space used to store the previously
	 * translated paths. */
	if (status > 0) {
		word_t child_ptr;
		child_ptr = resize_child_stack(pid, -status);
		if (child_ptr == 0)
			set_sysarg(pid, SYSARG_RESULT, (word_t)-EFAULT);
	}

	/* Translate output arguments. */
	switch (sysnum) {
	case __NR_getcwd:
		size = get_sysarg(pid, SYSARG_RESULT);
		if ((int)size < 0)
			return;

		status = detranslate_sysarg(pid, SYSARG_1, size, STRONG);
		if (status < 0)
			break;

		set_sysarg(pid, SYSARG_RESULT, (word_t)status);
		break;

	case __NR_readlink:
		size = get_sysarg(pid, SYSARG_RESULT);
		if ((int)size < 0)
			return;

		status = detranslate_sysarg(pid, SYSARG_2, size, WEAK);
		if (status < 0)
			break;

		set_sysarg(pid, SYSARG_RESULT, (word_t)status - 1);
		break;

	case __NR_readlinkat:
		size = get_sysarg(pid, SYSARG_RESULT);
		if ((int)size < 0)
			return;

		status = detranslate_sysarg(pid, SYSARG_3, size, WEAK);
		if (status < 0)
			break;

		set_sysarg(pid, SYSARG_RESULT, (word_t)status - 1);
		break;

	default:
		return;
	}

	if (status < 0)
		set_sysarg(pid, SYSARG_RESULT, (word_t)status);
}