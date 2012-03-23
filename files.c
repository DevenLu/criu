#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <linux/limits.h>

#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "crtools.h"

#include "files.h"
#include "image.h"
#include "list.h"
#include "util.h"
#include "util-net.h"
#include "lock.h"

static struct fdinfo_desc *fdinfo_descs;
static int nr_fdinfo_descs;

static struct fdinfo_list_entry *fdinfo_list;
static int nr_fdinfo_list;

static struct fmap_fd *fmap_fds;

int prepare_shared_fdinfo(void)
{
	fdinfo_descs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	if (fdinfo_descs == MAP_FAILED) {
		pr_perror("Can't map fdinfo_descs");
		return -1;
	}

	fdinfo_list = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	if (fdinfo_list == MAP_FAILED) {
		pr_perror("Can't map fdinfo_list");
		return -1;
	}
	return 0;
}

static struct fdinfo_desc *find_fd(u64 id)
{
	struct fdinfo_desc *fi;
	int i;

	for (i = 0; i < nr_fdinfo_descs; i++) {
		fi = fdinfo_descs + i;
		if (fi->id == id)
			return fi;
	}

	return NULL;
}

static int get_file_path(char *path, struct fdinfo_entry *fe, int fd)
{
	if (read(fd, path, fe->len) != fe->len) {
		pr_perror("Error reading path");
		return -1;
	}

	path[fe->len] = '\0';

	return 0;
}

static int collect_fd(int pid, struct fdinfo_entry *e)
{
	int i;
	struct fdinfo_list_entry *le = &fdinfo_list[nr_fdinfo_list];
	struct fdinfo_desc *desc;

	pr_info("Collect fdinfo pid=%d fd=%ld id=%16lx\n",
		pid, e->addr, e->id);

	nr_fdinfo_list++;
	if ((nr_fdinfo_list) * sizeof(struct fdinfo_list_entry) >= 4096) {
		pr_err("OOM storing fdinfo_list_entries\n");
		return -1;
	}

	le->pid = pid;
	le->fd = e->addr;
	le->real_pid = 0;

	for (i = 0; i < nr_fdinfo_descs; i++) {
		desc = &fdinfo_descs[i];

		if (desc->id != e->id)
			continue;

		fdinfo_descs[i].users++;
		list_add(&le->list, &desc->list);

		if (fdinfo_descs[i].pid < pid)
			return 0;

		desc->pid = pid;
		desc->addr = e->addr;

		return 0;
	}

	if ((nr_fdinfo_descs + 1) * sizeof(struct fdinfo_desc) >= 4096) {
		pr_err("OOM storing fdinfo descriptions\n");
		return -1;
	}

	desc = &fdinfo_descs[nr_fdinfo_descs];
	memzero(desc, sizeof(*desc));

	desc->id	= e->id;
	desc->addr	= e->addr;
	desc->pid	= pid;
	desc->users	= 1;
	INIT_LIST_HEAD(&desc->list);

	list_add(&le->list, &desc->list);
	nr_fdinfo_descs++;

	return 0;
}

int prepare_fd_pid(int pid)
{
	int fdinfo_fd, ret = 0;
	u32 type = 0;

	fdinfo_fd = open_image_ro(CR_FD_FDINFO, pid);
	if (fdinfo_fd < 0) {
		if (errno == ENOENT)
			return 0;
		else
			return -1;
	}

	while (1) {
		struct fdinfo_entry e;

		ret = read_img_eof(fdinfo_fd, &e);
		if (ret <= 0)
			break;

		if (e.len)
			lseek(fdinfo_fd, e.len, SEEK_CUR);

		if (fd_is_special(&e))
			continue;

		ret = collect_fd(pid, &e);
		if (ret < 0)
			break;
	}

	close(fdinfo_fd);
	return ret;
}

static int open_fe_fd(struct fdinfo_entry *fe, int fd)
{
	char path[PATH_MAX];
	int tmp;

	if (get_file_path(path, fe, fd))
		return -1;

	tmp = open(path, fe->flags);
	if (tmp < 0) {
		pr_perror("Can't open file %s", path);
		return -1;
	}

	lseek(tmp, fe->pos, SEEK_SET);

	return tmp;
}

static int restore_cwd(struct fdinfo_entry *fe, int fd)
{
	char path[PATH_MAX];
	int ret;

	if (get_file_path(path, fe, fd))
		return -1;

	pr_info("Restore CWD %s\n", path);
	ret = chdir(path);
	if (ret < 0) {
		pr_perror("Can't change dir %s", path);
		return -1;
	}

	return 0;
}

static int restore_exe_early(struct fdinfo_entry *fe, int fd)
{
	/*
	 * We restore the EXE symlink at very late stage
	 * because of restrictions applied from kernel side,
	 * so simply skip it for a while.
	 */
	lseek(fd, fe->len, SEEK_CUR);
	return 0;
}

struct fdinfo_list_entry *find_fdinfo_list_entry(int pid, int fd, struct fdinfo_desc *fi)
{
	struct fdinfo_list_entry *fle;
	int found = 0;

	list_for_each_entry(fle, &fi->list, list) {
		if (fle->fd == fd && fle->pid == pid) {
			found = 1;
			break;
		}
	}

	BUG_ON(found == 0);
	return fle;
}

static inline void transport_name_gen(struct sockaddr_un *addr, int *len,
		int pid, long fd)
{
	addr->sun_family = AF_UNIX;
	snprintf(addr->sun_path, UNIX_PATH_MAX, "x/crtools-fd-%d-%ld", pid, fd);
	*len = SUN_LEN(addr);
	*addr->sun_path = '\0';
}

static int open_transport_fd(int pid, struct fdinfo_entry *fe,
				struct fdinfo_desc *fi)
{
	struct fdinfo_list_entry *fle;
	struct sockaddr_un saddr;
	int sock;
	int ret, sun_len;

	if (fi->pid == pid)
		return 0;

	transport_name_gen(&saddr, &sun_len, getpid(), fe->addr);

	pr_info("\t%d: Create transport fd for %lx type %d namelen %d users %d\n", pid,
			(unsigned long)fe->addr, fe->type, fe->len, fi->users);

	fle = find_fdinfo_list_entry(pid, fe->addr, fi);

	sock = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		pr_perror("Can't create socket");
		return -1;
	}
	ret = bind(sock, &saddr, sun_len);
	if (ret < 0) {
		pr_perror("Can't bind unix socket %s", saddr.sun_path + 1);
		return -1;
	}

	ret = reopen_fd_as((int)fe->addr, sock);
	if (ret < 0)
		return -1;

	pr_info("Wake up fdinfo pid=%d fd=%d\n", fle->pid, fle->fd);
	cr_wait_set(&fle->real_pid, getpid());

	return 0;
}

static int open_fd(int pid, struct fdinfo_entry *fe,
				struct fdinfo_desc *fi, int fdinfo_fd)
{
	int tmp;
	int serv, sock;
	struct sockaddr_un saddr;
	struct fdinfo_list_entry *fle;

	if ((fi->pid != pid) || (fe->addr != fi->addr))
		return 0;

	tmp = open_fe_fd(fe, fdinfo_fd);
	if (tmp < 0)
		return -1;

	if (reopen_fd_as((int)fe->addr, tmp))
		return -1;

	if (fi->users == 1)
		goto out;

	sock = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		pr_perror("Can't create socket");
		return -1;
	}

	cr_wait_set(&fi->real_pid, getpid());

	pr_info("\t%d: Create fd for %lx type %d namelen %d users %d\n", pid,
			(unsigned long)fe->addr, fe->type, fe->len, fi->users);

	list_for_each_entry(fle, &fi->list, list) {
		int len;

		fi->users--;

		if (pid == fle->pid)
			continue;

		pr_info("Wait fdinfo pid=%d fd=%d\n", fle->pid, fle->fd);
		cr_wait_while(&fle->real_pid, 0);

		pr_info("Send fd %d to %s\n", (int)fe->addr, saddr.sun_path + 1);
		transport_name_gen(&saddr, &len, fle->real_pid, fle->fd);

		if (send_fd(sock, &saddr, len, fe->addr) < 0) {
			pr_perror("Can't send file descriptor");
			return -1;
		}
	}

	BUG_ON(fi->users);
	close(sock);
out:
	return 0;
}

static int receive_fd(int pid, struct fdinfo_entry *fe, struct fdinfo_desc *fi)
{
	int tmp;

	if (fi->pid == pid) {
		if (fi->addr != fe->addr) {
			tmp = dup2(fi->addr, fe->addr);
			if (tmp < 0) {
				pr_perror("Can't duplicate fd %ld %ld",
						fi->addr, fe->addr);
				return -1;
			}
		}

		return 0;
	}

	pr_info("\t%d: Receive fd for %lx type %d namelen %d users %d\n", pid,
			(unsigned long)fe->addr, fe->type, fe->len, fi->users);

	tmp = recv_fd(fe->addr);
	if (tmp < 0) {
		pr_err("Can't get fd %d\n", tmp);
		return -1;
	}
	close(fe->addr);

	return reopen_fd_as((int)fe->addr, tmp);
}

static int open_fmap(int pid, struct fdinfo_entry *fe, int fd)
{
	struct fmap_fd *new;
	int tmp;

	tmp = open_fe_fd(fe, fd);
	if (tmp < 0)
		return -1;

	pr_info("%d:\t\tWill map %lx to %d\n", pid, (unsigned long)fe->addr, tmp);

	new = xmalloc(sizeof(*new));
	if (!new) {
		close_safe(&tmp);
		return -1;
	}

	new->start	= fe->addr;
	new->fd		= tmp;
	new->next	= fmap_fds;
	new->pid	= pid;

	fmap_fds	= new;

	return 0;
}

static int open_fdinfo(int pid, struct fdinfo_entry *fe, int *fdinfo_fd, int state)
{
	u32 mag;
	int ret = 0;

	struct fdinfo_desc *fi = find_fd(fe->id);

	if (move_img_fd(fdinfo_fd, (int)fe->addr))
		return -1;

	pr_info("\t%d: Got fd for %lx type %d namelen %d users %d\n", pid,
			(unsigned long)fe->addr, fe->type, fe->len, fi->users);

	BUG_ON(fe->type != FDINFO_REG);


	switch (state) {
	case FD_STATE_PREP:
		ret = open_transport_fd(pid, fe, fi);
		break;
	case FD_STATE_CREATE:
		ret = open_fd(pid, fe, fi, *fdinfo_fd);
		break;
	case FD_STATE_RECV:
		ret = receive_fd(pid, fe, fi);
		break;
	}

	return ret;
}

static int open_special_fdinfo(int pid, struct fdinfo_entry *fe,
		int fdinfo_fd, int state)
{
	if (state != FD_STATE_RECV) {
		lseek(fdinfo_fd, fe->len, SEEK_CUR);
		return 0;
	}

	if (fe->type == FDINFO_MAP)
		return open_fmap(pid, fe, fdinfo_fd);
	if (fe->type == FDINFO_CWD)
		return restore_cwd(fe, fdinfo_fd);
	if (fe->type == FDINFO_EXE)
		return restore_exe_early(fe, fdinfo_fd);

	pr_info("%d: fe->type: %d\n", pid,  fe->type);
	BUG_ON(1);
	return -1;
}

int prepare_fds(int pid)
{
	u32 type = 0, err = -1, ret;
	int fdinfo_fd;
	int state;
	off_t offset, magic_offset;

	struct fdinfo_entry fe;
	int nr = 0;

	pr_info("%d: Opening fdinfo-s\n", pid);

	fdinfo_fd = open_image_ro(CR_FD_FDINFO, pid);
	if (fdinfo_fd < 0) {
		pr_perror("%d: Can't open pipes img", pid);
		return -1;
	}

	magic_offset = lseek(fdinfo_fd, 0, SEEK_CUR);

	for (state = 0; state < FD_STATE_MAX; state++) {
		lseek(fdinfo_fd, magic_offset, SEEK_SET);

		while (1) {
			ret = read(fdinfo_fd, &fe, sizeof(fe));
			if (ret == 0)
				break;

			if (ret != sizeof(fe)) {
				pr_perror("%d: Bad fdinfo entry", pid);
				goto err;
			}

			if (fd_is_special(&fe)) {
				if (open_special_fdinfo(pid, &fe, fdinfo_fd, state))
					goto err;

				continue;
			}

			offset = lseek(fdinfo_fd, 0, SEEK_CUR);

			if (open_fdinfo(pid, &fe, &fdinfo_fd, state))
				goto err;

			lseek(fdinfo_fd, offset + fe.len, SEEK_SET);
		}
	}
	err = 0;
err:
	close(fdinfo_fd);
	return err;
}

static struct fmap_fd *pull_fmap_fd(int pid, unsigned long start)
{
	struct fmap_fd **p, *r;

	pr_info("%d: Looking for %lx : ", pid, start);

	for (p = &fmap_fds; *p != NULL; p = &(*p)->next) {
		if ((*p)->start != start || (*p)->pid != pid)
			continue;

		r = *p;
		*p = r->next;
		pr_info("found\n");

		return r;
	}

	pr_info("not found\n");
	return NULL;
}

int get_filemap_fd(int pid, struct vma_entry *vma_entry)
{
	struct fmap_fd *fmap_fd;
	
	fmap_fd = pull_fmap_fd(pid, vma_entry->start);
	return fmap_fd ? fmap_fd->fd : -1;
}
