/*
 *  linux/fs/file.c
 *
 *  Copyright (C) 1998-1999, Stephen Tweedie and Bill Hawes
 *
 *  Manage the dynamic fd arrays in the process files_struct.
 */

#include <linux/export.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>

struct fdtable_defer {
	spinlock_t lock;
	struct work_struct wq;
	struct fdtable *next;
};

int sysctl_nr_open __read_mostly = 1024*1024;
int sysctl_nr_open_min = BITS_PER_LONG;
int sysctl_nr_open_max = 1024 * 1024; /* raised later */

/*
 * We use this list to defer free fdtables that have vmalloced
 * sets/arrays. By keeping a per-cpu list, we avoid having to embed
 * the work_struct in fdtable itself which avoids a 64 byte (i386) increase in
 * this per-task structure.
 */
static DEFINE_PER_CPU(struct fdtable_defer, fdtable_defer_list);

static void *alloc_fdmem(size_t size)
{
	/*
	 * Very large allocations can stress page reclaim, so fall back to
	 * vmalloc() if the allocation size will be considered "large" by the VM.
	 */
	if (size <= (PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER)) {
		void *data = kmalloc(size, GFP_KERNEL|__GFP_NOWARN|__GFP_NORETRY);
		if (data != NULL)
			return data;
	}
	return vmalloc(size);
}

static void free_fdmem(void *ptr)
{
	is_vmalloc_addr(ptr) ? vfree(ptr) : kfree(ptr);
}

static void __free_fdtable(struct fdtable *fdt)
{
	free_fdmem(fdt->fd);
	free_fdmem(fdt->open_fds);
	kfree(fdt);
}

static void free_fdtable_work(struct work_struct *work)
{
	struct fdtable_defer *f =
		container_of(work, struct fdtable_defer, wq);
	struct fdtable *fdt;

	spin_lock_bh(&f->lock);
	fdt = f->next;
	f->next = NULL;
	spin_unlock_bh(&f->lock);
	while(fdt) {
		struct fdtable *next = fdt->next;

		__free_fdtable(fdt);
		fdt = next;
	}
}

void free_fdtable_rcu(struct rcu_head *rcu)
{
	struct fdtable *fdt = container_of(rcu, struct fdtable, rcu);
	struct fdtable_defer *fddef;

	BUG_ON(!fdt);

	if (fdt->max_fds <= NR_OPEN_DEFAULT) {
		/*
		 * This fdtable is embedded in the files structure and that
		 * structure itself is getting destroyed.
		 */
		kmem_cache_free(files_cachep,
				container_of(fdt, struct files_struct, fdtab));
		return;
	}
	if (!is_vmalloc_addr(fdt->fd) && !is_vmalloc_addr(fdt->open_fds)) {
		kfree(fdt->fd);
		kfree(fdt->open_fds);
		kfree(fdt);
	} else {
		fddef = &get_cpu_var(fdtable_defer_list);
		spin_lock(&fddef->lock);
		fdt->next = fddef->next;
		fddef->next = fdt;
		/* vmallocs are handled from the workqueue context */
		schedule_work(&fddef->wq);
		spin_unlock(&fddef->lock);
		put_cpu_var(fdtable_defer_list);
	}
}

/*
 * Expand the fdset in the files_struct.  Called with the files spinlock
 * held for write.
 */
static void copy_fdtable(struct fdtable *nfdt, struct fdtable *ofdt)
{
	size_t cpy, set;

	BUG_ON(nfdt->max_fds < ofdt->max_fds);

	cpy = ofdt->max_fds * sizeof(struct file *);
	set = (nfdt->max_fds - ofdt->max_fds) * sizeof(struct file *);
	memcpy(nfdt->fd, ofdt->fd, cpy);
	memset((char *)(nfdt->fd) + cpy, 0, set);

	cpy = ofdt->max_fds / BITS_PER_BYTE;
	set = (nfdt->max_fds - ofdt->max_fds) / BITS_PER_BYTE;
	memcpy(nfdt->open_fds, ofdt->open_fds, cpy);
	memset((char *)(nfdt->open_fds) + cpy, 0, set);
	memcpy(nfdt->close_on_exec, ofdt->close_on_exec, cpy);
	memset((char *)(nfdt->close_on_exec) + cpy, 0, set);
}

static struct fdtable * alloc_fdtable(unsigned int nr)
{
	struct fdtable *fdt;
	void *data;

	/*
	 * Figure out how many fds we actually want to support in this fdtable.
	 * Allocation steps are keyed to the size of the fdarray, since it
	 * grows far faster than any of the other dynamic data. We try to fit
	 * the fdarray into comfortable page-tuned chunks: starting at 1024B
	 * and growing in powers of two from there on.
	 */
	nr /= (1024 / sizeof(struct file *));
	nr = roundup_pow_of_two(nr + 1);
	nr *= (1024 / sizeof(struct file *));
	/*
	 * Note that this can drive nr *below* what we had passed if sysctl_nr_open
	 * had been set lower between the check in expand_files() and here.  Deal
	 * with that in caller, it's cheaper that way.
	 *
	 * We make sure that nr remains a multiple of BITS_PER_LONG - otherwise
	 * bitmaps handling below becomes unpleasant, to put it mildly...
	 */
	if (unlikely(nr > sysctl_nr_open))
		nr = ((sysctl_nr_open - 1) | (BITS_PER_LONG - 1)) + 1;

	fdt = kmalloc(sizeof(struct fdtable), GFP_KERNEL);
	if (!fdt)
		goto out;
	fdt->max_fds = nr;
	data = alloc_fdmem(nr * sizeof(struct file *));
	if (!data)
		goto out_fdt;
	fdt->fd = data;

	data = alloc_fdmem(max_t(size_t,
				 2 * nr / BITS_PER_BYTE, L1_CACHE_BYTES));
	if (!data)
		goto out_arr;
	fdt->open_fds = data;
	data += nr / BITS_PER_BYTE;
	fdt->close_on_exec = data;
	fdt->next = NULL;

	return fdt;

out_arr:
	free_fdmem(fdt->fd);
out_fdt:
	kfree(fdt);
out:
	return NULL;
}

/*
 * Expand the file descriptor table.
 * This function will allocate a new fdtable and both fd array and fdset, of
 * the given size.
 * Return <0 error code on error; 1 on successful completion.
 * The files->file_lock should be held on entry, and will be held on exit.
 */
static int expand_fdtable(struct files_struct *files, int nr)
	__releases(files->file_lock)
	__acquires(files->file_lock)
{
	struct fdtable *new_fdt, *cur_fdt;

	spin_unlock(&files->file_lock);
	new_fdt = alloc_fdtable(nr);
	spin_lock(&files->file_lock);
	if (!new_fdt)
		return -ENOMEM;
	/*
	 * extremely unlikely race - sysctl_nr_open decreased between the check in
	 * caller and alloc_fdtable().  Cheaper to catch it here...
	 */
	if (unlikely(new_fdt->max_fds <= nr)) {
		__free_fdtable(new_fdt);
		return -EMFILE;
	}
	/*
	 * Check again since another task may have expanded the fd table while
	 * we dropped the lock
	 */
	cur_fdt = files_fdtable(files);
	if (nr >= cur_fdt->max_fds) {
		/* Continue as planned */
		copy_fdtable(new_fdt, cur_fdt);
		rcu_assign_pointer(files->fdt, new_fdt);
		if (cur_fdt->max_fds > NR_OPEN_DEFAULT)
			free_fdtable(cur_fdt);
	} else {
		/* Somebody else expanded, so undo our attempt */
		__free_fdtable(new_fdt);
	}
	return 1;
}

/*
 * Expand files.
 * This function will expand the file structures, if the requested size exceeds
 * the current capacity and there is room for expansion.
 * Return <0 error code on error; 0 when nothing done; 1 when files were
 * expanded and execution may have blocked.
 * The files->file_lock should be held on entry, and will be held on exit.
 */
int expand_files(struct files_struct *files, int nr)
{
	struct fdtable *fdt;

	fdt = files_fdtable(files);

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	if (nr >= rlimit(RLIMIT_NOFILE))
		return -EMFILE;

	/* Do we need to expand? */
	if (nr < fdt->max_fds)
		return 0;

	/* Can we expand? */
	if (nr >= sysctl_nr_open)
		return -EMFILE;

	/* All good, so we try */
	return expand_fdtable(files, nr);
}

static int count_open_files(struct fdtable *fdt)
{
	int size = fdt->max_fds;
	int i;

	/* Find the last open fd */
	for (i = size / BITS_PER_LONG; i > 0; ) {
		if (fdt->open_fds[--i])
			break;
	}
	i = (i + 1) * BITS_PER_LONG;
	return i;
}

/*
 * Allocate a new files structure and copy contents from the
 * passed in files structure.
 * errorp will be valid only when the returned files_struct is NULL.
 */
struct files_struct *dup_fd(struct files_struct *oldf, int *errorp)
{
	struct files_struct *newf;
	struct file **old_fds, **new_fds;
	int open_files, size, i;
	struct fdtable *old_fdt, *new_fdt;

	*errorp = -ENOMEM;
	newf = kmem_cache_alloc(files_cachep, GFP_KERNEL);
	if (!newf)
		goto out;

	atomic_set(&newf->count, 1);

	spin_lock_init(&newf->file_lock);
	newf->next_fd = 0;
	new_fdt = &newf->fdtab;
	new_fdt->max_fds = NR_OPEN_DEFAULT;
	new_fdt->close_on_exec = newf->close_on_exec_init;
	new_fdt->open_fds = newf->open_fds_init;
	new_fdt->fd = &newf->fd_array[0];
	new_fdt->next = NULL;

	spin_lock(&oldf->file_lock);
	old_fdt = files_fdtable(oldf);
	open_files = count_open_files(old_fdt);

	/*
	 * Check whether we need to allocate a larger fd array and fd set.
	 */
	while (unlikely(open_files > new_fdt->max_fds)) {
		spin_unlock(&oldf->file_lock);

		if (new_fdt != &newf->fdtab)
			__free_fdtable(new_fdt);

		new_fdt = alloc_fdtable(open_files - 1);
		if (!new_fdt) {
			*errorp = -ENOMEM;
			goto out_release;
		}

		/* beyond sysctl_nr_open; nothing to do */
		if (unlikely(new_fdt->max_fds < open_files)) {
			__free_fdtable(new_fdt);
			*errorp = -EMFILE;
			goto out_release;
		}

		/*
		 * Reacquire the oldf lock and a pointer to its fd table
		 * who knows it may have a new bigger fd table. We need
		 * the latest pointer.
		 */
		spin_lock(&oldf->file_lock);
		old_fdt = files_fdtable(oldf);
		open_files = count_open_files(old_fdt);
	}

	old_fds = old_fdt->fd;
	new_fds = new_fdt->fd;

	memcpy(new_fdt->open_fds, old_fdt->open_fds, open_files / 8);
	memcpy(new_fdt->close_on_exec, old_fdt->close_on_exec, open_files / 8);

	for (i = open_files; i != 0; i--) {
		struct file *f = *old_fds++;
		if (f) {
			get_file(f);
		} else {
			/*
			 * The fd may be claimed in the fd bitmap but not yet
			 * instantiated in the files array if a sibling thread
			 * is partway through open().  So make sure that this
			 * fd is available to the new process.
			 */
			__clear_open_fd(open_files - i, new_fdt);
		}
		rcu_assign_pointer(*new_fds++, f);
	}
	spin_unlock(&oldf->file_lock);

	/* compute the remainder to be cleared */
	size = (new_fdt->max_fds - open_files) * sizeof(struct file *);

	/* This is long word aligned thus could use a optimized version */
	memset(new_fds, 0, size);

	if (new_fdt->max_fds > open_files) {
		int left = (new_fdt->max_fds - open_files) / 8;
		int start = open_files / BITS_PER_LONG;

		memset(&new_fdt->open_fds[start], 0, left);
		memset(&new_fdt->close_on_exec[start], 0, left);
	}

	rcu_assign_pointer(newf->fdt, new_fdt);

	return newf;

out_release:
	kmem_cache_free(files_cachep, newf);
out:
	return NULL;
}

static void __devinit fdtable_defer_list_init(int cpu)
{
	struct fdtable_defer *fddef = &per_cpu(fdtable_defer_list, cpu);
	spin_lock_init(&fddef->lock);
	INIT_WORK(&fddef->wq, free_fdtable_work);
	fddef->next = NULL;
}

void __init files_defer_init(void)
{
	int i;
	for_each_possible_cpu(i)
		fdtable_defer_list_init(i);
	sysctl_nr_open_max = min((size_t)INT_MAX, ~(size_t)0/sizeof(void *)) &
			     -BITS_PER_LONG;
}

struct files_struct init_files = {
	.count		= ATOMIC_INIT(1),
	.fdt		= &init_files.fdtab,
	.fdtab		= {
		.max_fds	= NR_OPEN_DEFAULT,
		.fd		= &init_files.fd_array[0],
		.close_on_exec	= init_files.close_on_exec_init,
		.open_fds	= init_files.open_fds_init,
	},
	.file_lock	= __SPIN_LOCK_UNLOCKED(init_task.file_lock),
};

/*
 * allocate a file descriptor, mark it busy.
 */
int __alloc_fd(struct files_struct *files,
	       unsigned start, unsigned end, unsigned flags)
{
	unsigned int fd;
	int error;
	struct fdtable *fdt;

	spin_lock(&files->file_lock);
repeat:
	fdt = files_fdtable(files);
	fd = start;
	if (fd < files->next_fd)
		fd = files->next_fd;

	if (fd < fdt->max_fds)
		fd = find_next_zero_bit(fdt->open_fds, fdt->max_fds, fd);

	error = expand_files(files, fd);
	if (error < 0)
		goto out;

	/*
	 * If we needed to expand the fs array we
	 * might have blocked - try again.
	 */
	if (error)
		goto repeat;

	if (start <= files->next_fd)
		files->next_fd = fd + 1;

	__set_open_fd(fd, fdt);
	if (flags & O_CLOEXEC)
		__set_close_on_exec(fd, fdt);
	else
		__clear_close_on_exec(fd, fdt);
	error = fd;
#if 1
	/* Sanity check */
	if (rcu_dereference_raw(fdt->fd[fd]) != NULL) {
		printk(KERN_WARNING "alloc_fd: slot %d not NULL!\n", fd);
		rcu_assign_pointer(fdt->fd[fd], NULL);
	}
#endif

out:
	spin_unlock(&files->file_lock);
	return error;
}
EXPORT_SYMBOL(alloc_fd);

int alloc_fd(unsigned start, unsigned flags)
{
	return __alloc_fd(current->files, start, rlimit(RLIMIT_NOFILE), flags);
}

int get_unused_fd_flags(unsigned flags)
{
	return __alloc_fd(current->files, 0, rlimit(RLIMIT_NOFILE), flags);
}
EXPORT_SYMBOL(get_unused_fd_flags);

static void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
	struct fdtable *fdt = files_fdtable(files);
	__clear_open_fd(fd, fdt);
	if (fd < files->next_fd)
		files->next_fd = fd;
}

void put_unused_fd(unsigned int fd)
{
	struct files_struct *files = current->files;
	spin_lock(&files->file_lock);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
}

EXPORT_SYMBOL(put_unused_fd);

/*
 * Install a file pointer in the fd array.
 *
 * The VFS is full of places where we drop the files lock between
 * setting the open_fds bitmap and installing the file in the file
 * array.  At any such point, we are vulnerable to a dup2() race
 * installing a file in the array before us.  We need to detect this and
 * fput() the struct file we are about to overwrite in this case.
 *
 * It should never happen - if we allow dup2() do it, _really_ bad things
 * will follow.
 *
 * NOTE: __fd_install() variant is really, really low-level; don't
 * use it unless you are forced to by truly lousy API shoved down
 * your throat.  'files' *MUST* be either current->files or obtained
 * by get_files_struct(current) done by whoever had given it to you,
 * or really bad things will happen.  Normally you want to use
 * fd_install() instead.
 */

void __fd_install(struct files_struct *files, unsigned int fd,
		struct file *file)
{
	struct fdtable *fdt;
	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	BUG_ON(fdt->fd[fd] != NULL);
	rcu_assign_pointer(fdt->fd[fd], file);
	spin_unlock(&files->file_lock);
}

void fd_install(unsigned int fd, struct file *file)
{
	__fd_install(current->files, fd, file);
}

EXPORT_SYMBOL(fd_install);

/*
 * The same warnings as for __alloc_fd()/__fd_install() apply here...
 */
int __close_fd(struct files_struct *files, unsigned fd)
{
	struct file *file;
	struct fdtable *fdt;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	if (fd >= fdt->max_fds)
		goto out_unlock;
	file = fdt->fd[fd];
	if (!file)
		goto out_unlock;
	rcu_assign_pointer(fdt->fd[fd], NULL);
	__clear_close_on_exec(fd, fdt);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
	return filp_close(file, files);

out_unlock:
	spin_unlock(&files->file_lock);
	return -EBADF;
}

struct file *fget(unsigned int fd)
{
	struct file *file;
	struct files_struct *files = current->files;

	rcu_read_lock();
	file = fcheck_files(files, fd);
	if (file) {
		/* File object ref couldn't be taken */
		if (file->f_mode & FMODE_PATH ||
		    !atomic_long_inc_not_zero(&file->f_count))
			file = NULL;
	}
	rcu_read_unlock();

	return file;
}

EXPORT_SYMBOL(fget);

struct file *fget_raw(unsigned int fd)
{
	struct file *file;
	struct files_struct *files = current->files;

	rcu_read_lock();
	file = fcheck_files(files, fd);
	if (file) {
		/* File object ref couldn't be taken */
		if (!atomic_long_inc_not_zero(&file->f_count))
			file = NULL;
	}
	rcu_read_unlock();

	return file;
}

EXPORT_SYMBOL(fget_raw);

/*
 * Lightweight file lookup - no refcnt increment if fd table isn't shared.
 *
 * You can use this instead of fget if you satisfy all of the following
 * conditions:
 * 1) You must call fput_light before exiting the syscall and returning control
 *    to userspace (i.e. you cannot remember the returned struct file * after
 *    returning to userspace).
 * 2) You must not call filp_close on the returned struct file * in between
 *    calls to fget_light and fput_light.
 * 3) You must not clone the current task in between the calls to fget_light
 *    and fput_light.
 *
 * The fput_needed flag returned by fget_light should be passed to the
 * corresponding fput_light.
 */
struct file *fget_light(unsigned int fd, int *fput_needed)
{
	struct file *file;
	struct files_struct *files = current->files;

	*fput_needed = 0;
	if (atomic_read(&files->count) == 1) {
		file = fcheck_files(files, fd);
		if (file && (file->f_mode & FMODE_PATH))
			file = NULL;
	} else {
		rcu_read_lock();
		file = fcheck_files(files, fd);
		if (file) {
			if (!(file->f_mode & FMODE_PATH) &&
			    atomic_long_inc_not_zero(&file->f_count))
				*fput_needed = 1;
			else
				/* Didn't get the reference, someone's freed */
				file = NULL;
		}
		rcu_read_unlock();
	}

	return file;
}

struct file *fget_raw_light(unsigned int fd, int *fput_needed)
{
	struct file *file;
	struct files_struct *files = current->files;

	*fput_needed = 0;
	if (atomic_read(&files->count) == 1) {
		file = fcheck_files(files, fd);
	} else {
		rcu_read_lock();
		file = fcheck_files(files, fd);
		if (file) {
			if (atomic_long_inc_not_zero(&file->f_count))
				*fput_needed = 1;
			else
				/* Didn't get the reference, someone's freed */
				file = NULL;
		}
		rcu_read_unlock();
	}

	return file;
}
