/* TODO: provide to format any attached storage in embedfs and mount it
 *
 * The code below works only for the embedded flash at the moment. It would be
 * useful and convenient to format other storages, like eeprom and sdcard, in
 * embedfs too.
 *
 * 1. remove locks, place it somewhere else where it accesses for each devices
 * 2. each device can have a different block size
 */
#include <fs/fs.h>
#include <kernel/page.h>
#include <kernel/lock.h>
#include <kernel/buffer.h>
#include <kernel/device.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>
#include <bitops.h>
#include <stdbool.h>
#include "embedfs.h"

#define MAGIC				0xdeafc0de

#ifdef CONFIG_PAGING
#define BLOCK_SIZE			PAGESIZE
#else
#define BLOCK_SIZE			64
#endif

#define INODE_TABLE_SIZE(sz)		(100 * (sz)) /* 1% of disk size */
#define NAME_MAX			(256 - 1)
#define NR_INODE_MIN			16
#define NR_INODE_MAX			(BLOCK_SIZE * 8)

#define SUPERBLOCK			0
#define I_BMAP_BLK			1
#define D_BMAP_BLK			2

#define NR2ADDR(n)			(n * BLOCK_SIZE)

#define set_bitmap(n, arr)		arr[n / 8] |= 1 << (n % 8)
#define reset_bitmap(n, arr)		arr[n / 8] &= ~(1 << (n % 8))

#define get_inode_table(nr_inode, base)	\
	((nr_inode * sizeof(struct embed_inode) / BLOCK_SIZE) + base)
#define get_inode_table_offset(nr_inode) \
	(nr_inode * sizeof(struct embed_inode) % BLOCK_SIZE)

#define FT_ROOT				0xffff

static DEFINE_MUTEX(sb_lock);
static DEFINE_MUTEX(cr_lock);

static char *cached_bitmap;

static size_t read_block(unsigned int nr, void *buf, struct device *dev)
{
	unsigned int fs_block;
	char *diskbuf, *s, *d;
	size_t len = 0;

	fs_block = BASE_ALIGN(NR2ADDR(nr), BLOCK_SIZE);

	if ((diskbuf = getblk_lock(fs_block, dev)) == NULL)
		goto out;

	s = diskbuf;
	d = buf;
	len = BLOCK_SIZE;

	memcpy(d, s, len);

	putblk_unlock(fs_block, dev);

out:
	return len;
}

static size_t write_block(unsigned int nr, void *buf, size_t len,
		struct device *dev)
{
	unsigned int fs_block;
	char *diskbuf, *s, *d;

	fs_block = BASE_ALIGN(NR2ADDR(nr), BLOCK_SIZE);

	if ((diskbuf = getblk_lock(fs_block, dev)) == NULL) {
		len = 0;
		goto out;
	}

	s = buf;
	d = diskbuf;

	if ((d + len) > (diskbuf + BLOCK_SIZE))
		len -= (d + len) - (diskbuf + BLOCK_SIZE);

	memcpy(d, s, len);

	updateblk(fs_block, dev);

	putblk_unlock(fs_block, dev);

out:
	return len;
}

#ifdef CONFIG_DEBUG
static __attribute__((unused)) void print_block(unsigned int n, struct device *dev)
{
	char *buf;
	int i;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return;

	read_block(n, buf, dev);

	for (i = 1; i <= BLOCK_SIZE; i++) {
		printk("%02x ", buf[i-1]);
		if (!(i % 16))
			printk("\n");
	}
	printk("\n");

	kfree(buf);
}
#endif

static int read_superblock(struct embed_superblock *sb,
		struct device *dev)
{
	char *buf;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return -ENOMEM;

	read_block(SUPERBLOCK, buf, dev);
	memcpy(sb, buf, sizeof(struct embed_superblock));

	kfree(buf);

	return 0;
}

static void write_superblock(struct embed_superblock *sb,
		struct device *dev)
{
	write_block(SUPERBLOCK, sb, sizeof(struct embed_superblock), dev);
}

static void read_inode_bitmap(char *bitmap, struct device *dev)
{
	read_block(I_BMAP_BLK, bitmap, dev);
}

static void write_inode_bitmap(char *bitmap, struct device *dev)
{
	write_block(I_BMAP_BLK, bitmap, BLOCK_SIZE, dev);
}

static inline bool is_inode_free(unsigned int id)
{
	unsigned int i, bit;

	assert(cached_bitmap != NULL);

	i = id / 8;
	bit = id % 8;

	if (cached_bitmap[i] & (1 << bit))
		return false;

	return true;
}

static void free_inode(unsigned int id, struct device *dev)
{
	struct embed_superblock *sb;
	char *bitmap;

	if (is_inode_free(id))
		return;

	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		return;
	if ((bitmap = kmalloc(BLOCK_SIZE)) == NULL)
		goto out_free_sb;

	mutex_lock(&sb_lock);

	read_superblock(sb, dev);
	read_inode_bitmap(bitmap, dev);

	reset_bitmap(id, bitmap);
	sb->free_inodes_count++;

	write_superblock(sb, dev);
	write_inode_bitmap(bitmap, dev);

	if (cached_bitmap)
		kfree(cached_bitmap);
	cached_bitmap = bitmap;

	mutex_unlock(&sb_lock);

	assert(is_inode_free(id) == true);

out_free_sb:
	kfree(sb);
}

static unsigned int alloc_free_inode(struct device *dev)
{
	struct embed_superblock *sb;
	unsigned int i, bit, n, len;
	char *bitmap;

	n = 0;

	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		goto out;
	if ((bitmap = kmalloc(BLOCK_SIZE)) == NULL)
		goto out_free_sb;

	mutex_lock(&sb_lock);

	read_superblock(sb, dev);
	read_inode_bitmap(bitmap, dev);

	len = ALIGN(sb->nr_inodes, 8) / 8;
	for (i = 0; (i < len) && (bitmap[i] == 0xff); i++) ;
	bit = fls(bitmap[i]);

	if (bit >= 8 || i >= len ||
			((i == (len - 1)) && (bit > (sb->nr_inodes % 8)))) {
		cached_bitmap = bitmap;
		goto out_update_cache;
	}

	n = i * 8 + bit;

	set_bitmap(n, bitmap);
	sb->free_inodes_count--;
	write_superblock(sb, dev);
	write_inode_bitmap(bitmap, dev);

out_update_cache:
	if (cached_bitmap)
		kfree(cached_bitmap);
	cached_bitmap = bitmap;

	mutex_unlock(&sb_lock);

out_free_sb:
	kfree(sb);
out:
	return n; /* if zero, it failed to alloc an inode */
}

static int return_free_block(unsigned int nblock, struct device *dev)
{
	struct embed_superblock *sb;
	unsigned int i, j;
	char *bitmap;
	int ret = -ENOMEM;

	if ((bitmap = kmalloc(BLOCK_SIZE)) == NULL)
		goto out;
	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		goto out_free_bitmap;

	read_superblock(sb, dev);

	nblock -= sb->data_block;
	i = nblock / (BLOCK_SIZE * 8);
	j = nblock - (i * BLOCK_SIZE);

	read_block(D_BMAP_BLK + i, bitmap, dev);
	reset_bitmap(j, bitmap);
	write_block(D_BMAP_BLK + i, bitmap, BLOCK_SIZE, dev);

	sb->free_blocks_count++;
	write_superblock(sb, dev);

	ret = 0;

	kfree(sb);
out_free_bitmap:
	kfree(bitmap);
out:
	return ret;
}

static unsigned int alloc_free_block(struct device *dev)
{
	struct embed_superblock *sb;
	char *bitmap;
	unsigned int data_bitmap_size;
	unsigned int bit, i, j, n;

	n = 0;

	if ((bitmap = kmalloc(BLOCK_SIZE)) == NULL)
		goto out;
	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		goto out_free_bitmap;

	read_superblock(sb, dev);

	data_bitmap_size = ALIGN(sb->nr_blocks, 8) / 8;

	for (i = 0; i < data_bitmap_size; i++) {
		read_block(D_BMAP_BLK + i, bitmap, dev);

		for (j = 0; (j < BLOCK_SIZE) && (bitmap[j] == 0xff); j++) ;
		bit = fls(bitmap[j]);

		if (bit < 8 && j < BLOCK_SIZE) {
			if (i == (data_bitmap_size - 1) &&
					bit > (sb->nr_blocks % 8))
				break;

			n = j * 8 + bit;

			set_bitmap(n, bitmap);
			write_block(D_BMAP_BLK + i, bitmap, BLOCK_SIZE, dev);

			n += BLOCK_SIZE * 8 * i;
			break;
		}
	}

	if (n) {
		sb->free_blocks_count--;
		write_superblock(sb, dev);

		n += sb->data_block;
	}

	kfree(sb);
out_free_bitmap:
	kfree(bitmap);
out:
	return n;
}

static int update_inode_table(struct embed_inode *inode,
		struct device *dev)
{
	struct embed_superblock *sb;
	char *buf;
	unsigned int nblock, offset;
	int ret = -ENOMEM;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		goto out;
	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		goto out_free_buf;

	read_superblock(sb, dev);

	nblock = get_inode_table(inode->id, sb->inode_table);
	offset = get_inode_table_offset(inode->id);

	unsigned int diff = sizeof(struct embed_inode);

	if ((offset + sizeof(struct embed_inode)) > BLOCK_SIZE) {
		diff = BLOCK_SIZE - offset;
		read_block(nblock+1, buf, dev);
		memcpy(buf, (char *)inode + diff,
				sizeof(struct embed_inode) - diff);
		write_block(nblock+1, buf, BLOCK_SIZE, dev);
	}

	read_block(nblock, buf, dev);
	memcpy(buf+offset, inode, diff);
	write_block(nblock, buf, BLOCK_SIZE, dev);

	ret = 0;

	kfree(sb);
out_free_buf:
	kfree(buf);
out:
	return ret;
}

static inline unsigned int alloc_zeroed_free_block(struct device *dev)
{
	unsigned int nblock;
	char *buf;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return 0;

	if (!(nblock = alloc_free_block(dev)))
		goto out;

	memset(buf, 0, BLOCK_SIZE);
	write_block(nblock, buf, BLOCK_SIZE, dev);
out:
	kfree(buf);

	return nblock;
}

static inline unsigned int check_n_alloc_block(unsigned int nblock,
		unsigned int index, struct device *dev)
{
	unsigned int *buf;

	if (!nblock)
		return 0;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return 0;

	if (!read_block(nblock, buf, dev)) {
		nblock = 0;
		goto out;
	}

	if (!buf[index] || buf[index] == (unsigned int)-1) {
		if (!(buf[index] = alloc_zeroed_free_block(dev))) {
			nblock = 0;
			goto out; /* disk full */
		}

		write_block(nblock, buf, BLOCK_SIZE, dev);
	}

	nblock = buf[index];

out:
	kfree(buf);

	return nblock;
}

static inline unsigned int take_dblock(struct embed_inode *inode,
		unsigned int pos, struct device *dev)
{
	unsigned int nblock, index;

	index = pos / BLOCK_SIZE;

	if (index < NR_DATA_BLOCK_DIRECT) {
		if (!(nblock = inode->data[index]) &&
				(nblock = alloc_zeroed_free_block(dev))) {
			inode->data[index] = nblock;
			update_inode_table(inode, dev);
		}

		goto out;
	}

	unsigned int nr_entry, depth, length, i;

	index -= NR_DATA_BLOCK_DIRECT;
	nr_entry = BLOCK_SIZE / WORD_SIZE;
	length = 1;

#define mylog2(n)	(fls(n) - 1)
	for (depth = 0; index >> (mylog2(nr_entry) * (depth+1)); depth++) {
		length = 1 << (mylog2(nr_entry) * (depth+1));
		index -= length;
	}

	if (depth >= NR_DATA_BLOCK - NR_DATA_BLOCK_DIRECT) {
		nblock = 0;
		goto out;
	}

	nblock = inode->data[depth + NR_DATA_BLOCK_DIRECT];

	if (!nblock) {
		if (!(nblock = alloc_zeroed_free_block(dev)))
			goto out;

		inode->data[depth + NR_DATA_BLOCK_DIRECT] = nblock;
		update_inode_table(inode, dev);
	}

	for (i = 0; i <= depth; i++) {
		if (!(nblock = check_n_alloc_block(nblock,
						(index/length) % nr_entry,
						dev)))
			goto out;

		index %= length;
		length = length >> (fls(nr_entry) - 1);
	}

out:
	return nblock;
}

static int write_data_block(struct embed_inode *inode, unsigned int pos,
		const void *data, size_t len, struct device *dev)
{
	unsigned int nblk, pblk, off;
	size_t left;
	char *buf, *src;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return 0;

	src = (char *)data;
	pblk = nblk = 0;

	while (len) {
		/* exclusive access guaranteed as its inode is already locked
		 * and it will take the device lock through buffer-cache. */
		mutex_lock(&sb_lock);
		nblk = take_dblock(inode, pos, dev);
		mutex_unlock(&sb_lock);

		if (!nblk) /* disk full */
			break;

		if (nblk != pblk) {
			/* write back before reading the next block */
			if (pblk)
				write_block(pblk, buf, BLOCK_SIZE, dev);

			read_block(nblk, buf, dev);
		}

		off = pos % BLOCK_SIZE;
		left = BLOCK_SIZE - off;
		left = min(len, left);

		memcpy(buf + off, src, left);

		src += left;
		len -= left;
		pos += left;

		pblk = nblk;
	}

	if (nblk)
		write_block(nblk, buf, BLOCK_SIZE, dev);

	kfree(buf);

	return (unsigned int)src - (unsigned int)data;
}

static int read_data_block(struct embed_inode *inode, unsigned int pos,
		void *buf, size_t len, struct device *dev)
{
	unsigned int blk;
	size_t left;
	char *s, *d, *t;

	if ((t = kmalloc(BLOCK_SIZE)) == NULL)
		return -ENOMEM;

	if ((pos + len) > (inode->size + inode->hole))
		len -= (pos + len) - (inode->size + inode->hole);

	d = (char *)buf;

	while (len) {
		if (!(blk = take_dblock(inode, pos, dev))) {
			kfree(t);
			return -ERANGE;
		}

		left = BLOCK_SIZE - pos % BLOCK_SIZE;
		left = min(len, left);

		read_block(blk, t, dev);
		s = t + pos % BLOCK_SIZE;
		memcpy(d, s, left);

		d += left;
		pos += left;
		len -= left;
	}

	kfree(t);

	return (int)((unsigned int)d - (unsigned int)buf);
}

static unsigned int make_node(int mode, struct device *dev)
{
	unsigned int inode, i;
	struct embed_inode *new;

	if (!(inode = alloc_free_inode(dev))) {
		if (GET_FILE_TYPE(mode) != FT_ROOT) {
			error("embedfs: out of inode!!");
			return 0;
		}
	}

	if ((new = kmalloc(sizeof(struct embed_inode))) == NULL)
		return 0;

	new->id = inode;
	new->mode = mode;
	new->size = 0;
	new->hole = 0;
	for (i = 0; i < NR_DATA_BLOCK; i++) new->data[i] = 0;

	mutex_lock(&sb_lock);
	update_inode_table(new, dev);
	mutex_unlock(&sb_lock);

	kfree(new);

	return inode;
}

static int read_inode(struct embed_inode *inode, struct device *dev)
{
	struct embed_superblock *sb;
	unsigned int nblock, offset;
	char *buf;
	unsigned int inode_size;
	int ret = -ENOMEM;

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		goto out;
	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		goto out_free_buf;

	mutex_lock(&sb_lock);

	if (read_superblock(sb, dev))
		goto out_free_sb;

	inode_size = sizeof(struct embed_inode);

	nblock = inode->id * inode_size / BLOCK_SIZE;
	nblock += sb->inode_table;
	offset = inode->id * inode_size % BLOCK_SIZE;

	unsigned int diff = inode_size;

	if ((offset + inode_size) > BLOCK_SIZE) {
		diff = BLOCK_SIZE - offset;
		read_block(nblock+1, buf, dev);
		memcpy((char *)inode + diff, buf, inode_size - diff);
	}

	read_block(nblock, buf, dev);
	memcpy(inode, buf+offset, diff);

	ret = 0;

out_free_sb:
	mutex_unlock(&sb_lock);

	kfree(sb);
out_free_buf:
	kfree(buf);
out:
	return ret;
}

static size_t tok_strlen(const char *s, const char token)
{
	const char *p;

	for (p = s; *p; p++) {
		if (*p == token) {
			p++;
			break;
		}
	}

	return p - s;
}

static const char *lookup(const char *pathname, struct embed_inode *inode,
		unsigned short int *parent_id, struct device *dev)
{
	struct embed_dir *dir;
	struct embed_inode *curr;
	unsigned int pos, len, i, dir_size;
	const char *pwd;
	char *name;
	unsigned short int parent;

	if ((dir = kmalloc(sizeof(struct embed_dir))) == NULL)
		goto out;
	if ((curr = kmalloc(sizeof(struct embed_inode))) == NULL)
		goto out_free_dir;

	dir_size = sizeof(struct embed_dir) - sizeof(char *);

	/* skip '/'s if exist */
	for (i = 0; pathname[i] == '/'; i++) ;

	curr->id = parent = 0; /* start searching from root */
	if (read_inode(curr, dev)) {
		error("can not read root inode");
		goto out_free_inode;
	}

	for (pwd = pathname + i; *pwd; pwd = pathname + i) {
		len = tok_strlen(pwd, '/');

		/* if not a directory, it must be at the end of path. */
		if (!(curr->mode & FT_DIR))
			break;

		for (pos = 0; pos < (curr->size + curr->hole); pos += dir->rec_len) {
			read_data_block(curr, pos, dir, dir_size, dev);

			if (dir->type == FT_DELETED)
				continue;

			if (is_inode_free(dir->inode)) { /* not valid data */
				debug("not valid inode %x", dir->inode);
				goto out_free_inode;
			}

			if ((name = kmalloc(dir->name_len)) == NULL)
				goto out_free_inode;

			read_data_block(curr, pos + dir_size, name,
					dir->name_len, dev);

			if (len == dir->name_len-1 &&
					!strncmp(name, pwd, len)) {
				kfree(name);
				break;
			}

			kfree(name);
		}

		/* no such path exist if it reaches at the end of dir list */
		if (pos >= (curr->size + curr->hole))
			break;

		/* or move on */
		parent = curr->id;
		curr->id = dir->inode;
		if (read_inode(curr, dev)) {
			error("can not read inode, %x", curr->id);
		}

		i += len;
	}

	memcpy(inode, curr, sizeof(struct embed_inode));
	if (parent_id)
		*parent_id = parent;

	/* if pwd is null after all, the inode you're looking for is found.
	 * or remained path indicates broken, no such directory. return the
	 * last one found. */

	kfree(curr);
	kfree(dir);

	return pathname + i;

out_free_inode:
	kfree(curr);
out_free_dir:
	kfree(dir);
out:
	return (char *)-ENOMEM;
}

static unsigned int find_hole(struct embed_inode *inode,
		unsigned short int *size, struct device *dev)
{
	struct embed_dir *dir;
	unsigned int pos, dir_size;

	if (!(inode->mode & FT_DIR))
		return -EINVAL;

	if ((dir = kmalloc(sizeof(*dir))) == NULL)
		return -ENOMEM;

	dir_size = sizeof(*dir) - sizeof(char *);

	for (pos = 0; pos < (inode->size + inode->hole); pos += dir->rec_len) {
		read_data_block(inode, pos, dir, dir_size, dev);

		if (dir->type == FT_DELETED && dir->rec_len >= *size) {
			/* NOTE: wasted as much as dir->rec_len - *size */
			*size = dir->rec_len;
			inode->hole -= *size;
			break;
		}
	}

	kfree(dir);

	return pos;
}

static int create_file(const char *filename, int mode,
		struct embed_inode *parent, struct device *dev)
{
	struct embed_dir *dir;
	unsigned int inode_new, pos_next;
	int ret = 0;

	if ((dir = kmalloc(sizeof(struct embed_dir))) == NULL)
		return -ENOMEM;

	if (!(inode_new = make_node(mode, dev))) {
		ret = -ERANGE;
		goto out;
	}

	dir->inode = inode_new;
	dir->name_len = strnlen(filename, FILENAME_MAX) + 1;
	dir->rec_len = sizeof(struct embed_dir) + dir->name_len - sizeof(char *);
	dir->type = GET_FILE_TYPE(mode);
	dir->name = (char *)filename;

	pos_next = find_hole(parent, &dir->rec_len, dev);
	if (pos_next >= 0) {
		free_inode(inode_new, dev);
		goto out;
	}

	pos_next += write_data_block(parent, pos_next, dir,
			sizeof(struct embed_dir) - sizeof(char *), dev);
	pos_next += write_data_block(parent, pos_next, filename,
			dir->name_len, dev);

	mutex_lock(&sb_lock);
	parent->size += dir->rec_len;
	update_inode_table(parent, dev);
	mutex_unlock(&sb_lock);

out:
	kfree(dir);

	return ret;
}

static size_t embed_read_core(struct file *file, void *buf, size_t len)
{
	struct embed_inode *inode;
	int ret;

	if ((inode = kmalloc(sizeof(struct embed_inode))) == NULL) {
		ret = 0;
		goto out;
	}

	mutex_lock(&file->inode->lock);

	inode->id = file->inode->addr;
	if (read_inode(inode, file->inode->sb->dev)) {
		ret = 0;
		goto out_free_inode;
	}

	ret = read_data_block(inode, file->offset, buf, len,
			file->inode->sb->dev);

	if (ret > 0)
		file->offset += ret;

out_free_inode:
	kfree(inode);

	mutex_unlock(&file->inode->lock);
out:
	syscall_delegate_return(current->parent, ret);
	return ret;
}

static size_t embed_read(struct file *file, void *buf, size_t len)
{
	struct task *thread;

	if ((thread = make(TASK_HANDLER | STACK_SHARED, STACK_SIZE_DEFAULT,
					embed_read_core, current)) == NULL)
		return -ENOMEM;

	syscall_put_arguments(thread, file, buf, len, NULL);
	syscall_delegate(current, thread);

	return 0;
}

static size_t embed_write_core(struct file *file, void *buf, size_t len)
{
	struct embed_inode *inode;
	int ret;

	if ((inode = kmalloc(sizeof(struct embed_inode))) == NULL) {
		ret = 0;
		goto out;
	}

	mutex_lock(&file->inode->lock);

	inode->id = file->inode->addr;
	if (read_inode(inode, file->inode->sb->dev)) {
		ret = 0;
		goto out_free_inode;
	}

	ret = write_data_block(inode, file->offset, buf, len,
			file->inode->sb->dev);

	file->offset += ret;

	mutex_lock(&sb_lock);
	if (file->offset > inode->size) {
		inode->size = file->offset;
		file->inode->size = inode->size;
	}
	update_inode_table(inode, file->inode->sb->dev);
	mutex_unlock(&sb_lock);

	if (!ret)
		error("embedfs: disk full!\n");

out_free_inode:
	kfree(inode);

	mutex_unlock(&file->inode->lock);
out:
	syscall_delegate_return(current->parent, ret);
	return ret;
}

static size_t embed_write(struct file *file, void *buf, size_t len)
{
	struct task *thread;

	if ((thread = make(TASK_HANDLER | STACK_SHARED, STACK_SIZE_DEFAULT,
					embed_write_core, current)) == NULL)
		return -ENOMEM;

	syscall_put_arguments(thread, file, buf, len, NULL);
	syscall_delegate(current, thread);

	return 0;
}

static int embed_seek(struct file *file, unsigned int offset, int whence)
{
	switch (whence) {
	case SEEK_SET:
		file->offset = offset;
		break;
	case SEEK_CUR:
		if (file->offset + offset < file->offset)
			return -ERANGE;

		file->offset += offset;
		break;
	case SEEK_END:
		if ((file->inode->size - offset) < 0)
			return -ERANGE;

		file->offset = file->inode->size - offset;
		break;
	}

	return file->offset;
}

static int embed_lookup(struct inode *inode, const char *pathname)
{
	struct embed_inode *embed_inode;
	const char *s;
	int ret = 0;

	if ((embed_inode = kmalloc(sizeof(struct embed_inode))) == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if ((int)(s = lookup(pathname, embed_inode, NULL, inode->sb->dev)) < 0) {
		ret = (int)s;
		goto out_free_inode;
	}

	if (*s) {
		ret = -ENOENT;
		goto out_free_inode;
	}

	mutex_lock(&inode->lock);
	inode->addr = embed_inode->id;
	inode->mode = embed_inode->mode;
	inode->size = embed_inode->size;
	mutex_unlock(&inode->lock);

out_free_inode:
	kfree(embed_inode);
out:
	return ret;
}

static int embed_create(struct inode *inode, const char *pathname, int mode)
{
	struct embed_inode *embed_inode;
	const char *s;
	int ret;

	/* Lock first before allocating. This is less efficient but more
	 * avoidable from memory shortage. */
	mutex_lock(&cr_lock);
	/* FIXME: check again if the pathname still doesn't exist */

	if ((embed_inode = kmalloc(sizeof(struct embed_inode))) == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if ((int)(s = lookup(pathname, embed_inode, NULL, inode->sb->dev)) < 0) {
		ret = (int)s;
		goto out_free_inode;
	}

	if (!*s || toknum(s, "/")) {
		ret = -EEXIST;
		goto out_free_inode;
	}

	if (create_file(s, mode, embed_inode, inode->sb->dev)) {
		ret = -EEXIST;
		goto out_free_inode;
	}

	ret = embed_lookup(inode, pathname);

out_free_inode:
	kfree(embed_inode);
out:
	mutex_unlock(&cr_lock);

	return ret;
}

static void do_embed_close(struct file *file)
{
	mutex_lock(&file->inode->lock);
	__sync(file->inode->sb->dev);
	mutex_unlock(&file->inode->lock);
	rmfile(file);

	syscall_delegate_return(current->parent, 0);
}

static int embed_close(struct file *file)
{
	struct task *thread;

	if ((thread = make(TASK_HANDLER | STACK_SHARED, STACK_SIZE_DEFAULT,
					do_embed_close, current)) == NULL)
		return -ENOMEM;

	syscall_put_arguments(thread, file, NULL, NULL, NULL);
	syscall_delegate(current, thread);

	return SYSCALL_DEFERRED_WORK;
}

static inline void return_data_blocks(unsigned int *buf, struct device *dev)
{
	int i;

	for (i = 0; i < (BLOCK_SIZE / sizeof(*buf)); i++) {
		if (buf[i])
			return_free_block(buf[i], dev);
	}
}

/* FIXME: remove the recursive */
static inline void traverse(int depth, unsigned int *table, struct device *dev)
{
	unsigned int *buf, i;

	if (!depth) {
		return_data_blocks(table, dev);
		return;
	}

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return;

	for (i = 0; i < (BLOCK_SIZE / sizeof(*buf)); i++) {
		if (!table[i] || !read_block(table[i], buf, dev))
			continue;

		traverse(depth - 1, buf, dev);
		return_free_block(table[i], dev);
		table[i] = 0;
	}

	kfree(buf);
}

static inline void free_datablk(struct embed_inode *inode, struct device *dev)
{
	int i, depth;
	unsigned int *buf;

	for (i = 0; i < NR_DATA_BLOCK_DIRECT; i++) {
		if (inode->data[i]) {
			return_free_block(inode->data[i], dev);
			inode->data[i] = 0;
		}
	}

	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		return;

	for (i = NR_DATA_BLOCK_DIRECT; i < NR_DATA_BLOCK; i++) {
		depth = i - NR_DATA_BLOCK_DIRECT;

		if (!inode->data[i] || !read_block(inode->data[i], buf, dev))
			continue;

		traverse(depth, buf, dev);

		return_free_block(inode->data[i], dev);
		inode->data[i] = 0;
	}

	update_inode_table(inode, dev);
	kfree(buf);
}

static int embed_delete_core(struct embed_inode *inode, struct device *dev)
{
	struct embed_inode *child;
	struct embed_dir *dir;
	unsigned int pos, dir_size;

	if (inode->mode == FT_FILE) {
		free_datablk(inode, dev);
		free_inode(inode->id, dev);
		return 0;
	} else if (inode->mode != FT_DIR) {
		return -EPERM;
	}

	if ((dir = kmalloc(sizeof(*dir))) == NULL)
		goto out;
	if ((child = kmalloc(sizeof(*child))) == NULL)
		goto out_free_dir;

	dir_size = sizeof(*dir) - sizeof(char *);

	for (pos = 0; pos < (inode->size + inode->hole); pos += dir->rec_len) {
		read_data_block(inode, pos, dir, dir_size, dev);

		child->id = dir->inode;
		if (read_inode(child, dev)) {
			error("can not read inode");
			break;
		}

		if (dir->type == FT_DIR)
			/* FIXME: remove the recursive */
			embed_delete_core(child, dev);

		dir->type = FT_DELETED;
		free_datablk(child, dev);
		free_inode(child->id, dev);
	}

	free_datablk(inode, dev);
	free_inode(inode->id, dev);

	kfree(child);
	kfree(dir);

	return 0;

out_free_dir:
	kfree(dir);
out:
	return -ENOMEM;
}

static inline void delete_dir_entry(unsigned short int target,
		unsigned short int parent, struct device *dev)
{
	struct embed_inode *inode;
	struct embed_dir *dir;
	unsigned int pos, dir_size;

	if ((inode = kmalloc(sizeof(*inode))) == NULL)
		goto out_err_alloc;
	if ((dir = kmalloc(sizeof(*dir))) == NULL)
		goto out_free_inode;

	inode->id = parent;
	if (read_inode(inode, dev))
		goto out_err_read;

	dir_size = sizeof(*dir) - sizeof(char *);

	for (pos = 0; pos < (inode->size + inode->hole); pos += dir->rec_len) {
		read_data_block(inode, pos, dir, dir_size, dev);

		if (dir->inode != target)
			continue;

		dir->type = FT_DELETED;
		write_data_block(inode, pos, dir, sizeof(*dir), dev);

		mutex_lock(&sb_lock);
		inode->hole += dir->rec_len;
		inode->size -= dir->rec_len;
		update_inode_table(inode, dev);
		mutex_unlock(&sb_lock);
		break;
	}

	kfree(dir);
	kfree(inode);
	return;

out_err_read:
	error("can not read inode %x", inode->id);
out_free_inode:
	kfree(inode);
out_err_alloc:
	error("failed allocating");
}

static int embed_delete(struct inode *inode, const char *pathname)
{
	struct embed_inode *embed_inode;
	const char *s;
	unsigned short int parent_id = 0;
	int ret = 0;

	if ((embed_inode = kmalloc(sizeof(struct embed_inode))) == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if ((int)(s = lookup(pathname, embed_inode, &parent_id,
					inode->sb->dev)) < 0) {
		ret = (int)s;
		goto out_free_inode;
	}

	if (*s) {
		ret = -ENOENT;
		goto out_free_inode;
	}

	embed_delete_core(embed_inode, inode->sb->dev);
	delete_dir_entry(embed_inode->id, parent_id, inode->sb->dev);

out_free_inode:
	kfree(embed_inode);
out:
	__sync(inode->sb->dev);

	return ret;
}

static struct inode_operations iops = {
	.lookup = embed_lookup,
	.create = embed_create,
	.delete = embed_delete,
};

static struct file_operations fops = {
	.open  = NULL,
	.read  = embed_read,
	.write = embed_write,
	.close = embed_close,
	.seek  = embed_seek,
	.ioctl = NULL,
};

static void embed_read_inode(struct inode *inode)
{
	struct embed_inode *embed_inode;

	if ((embed_inode = kmalloc(sizeof(struct embed_inode))) == NULL)
		goto out;

	embed_inode->id = inode->addr;

	if (read_inode(embed_inode, inode->sb->dev) ||
			(inode->addr != embed_inode->id)) {
		kfree(embed_inode);
		goto out;
	}

	inode->mode = embed_inode->mode;
	inode->size = embed_inode->size;
	inode->iop = &iops;
	inode->fop = &fops;

	kfree(embed_inode);

	return;

out:
	inode->iop = NULL;
	inode->fop = NULL;
}

static int build_file_system(struct device *dev)
{
	unsigned int disk_size, nr_inodes;

	disk_size = dev->block_size * dev->nr_blocks;
	nr_inodes = disk_size / INODE_TABLE_SIZE(sizeof(struct embed_inode));
	if (nr_inodes < NR_INODE_MIN)
		nr_inodes = NR_INODE_MIN;
	else if (nr_inodes > NR_INODE_MAX)
		nr_inodes = NR_INODE_MAX;

	unsigned int nr_blocks;
	unsigned int data_bitmap_size; /* by byte */
	unsigned int inode_table_size_by_block;
	unsigned int inode_table; /* the start block number of the inode table */
	unsigned int data_block; /* the start block number of the first data
				    block */

	nr_blocks = disk_size / BLOCK_SIZE;
	data_bitmap_size = ALIGN(nr_blocks, 8) / 8;
	inode_table = data_bitmap_size / BLOCK_SIZE + 1;
	inode_table += 2;

	inode_table_size_by_block = ALIGN(sizeof(struct embed_inode) *
			nr_inodes, BLOCK_SIZE) / BLOCK_SIZE + 1;

	data_block = inode_table + inode_table_size_by_block;

	notice("# Building embedded file system\n"
	       "disk size %d\n"
	       "the number of blocks %d\n"
	       "the number of inodes %d\n"
	       "data bitmap size %d\n"
	       "inode table size %d",
	       disk_size,
	       nr_blocks,
	       nr_inodes,
	       data_bitmap_size,
	       inode_table_size_by_block);

	struct embed_superblock *sb;
	char *buf, *data_bitmap;
	int ret = -ENOMEM;

	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		goto out;
	if ((buf = kmalloc(BLOCK_SIZE)) == NULL)
		goto out_free_sb;
	if ((data_bitmap = kmalloc(data_bitmap_size)) == NULL)
		goto out_free_buf;

	sb->block_size = BLOCK_SIZE;
	sb->nr_blocks  = nr_blocks;
	sb->nr_inodes  = nr_inodes;
	sb->free_inodes_count = nr_inodes;
	sb->free_blocks_count = nr_blocks - data_block;
	sb->inode_table = inode_table;
	sb->data_block  = data_block;
	sb->first_block = dev->base_addr;
	sb->magic = MAGIC;

	write_superblock(sb, dev);

	unsigned int allocated, i;

	/* set bitmaps for blocks already allocated; the superblock, the inode
	 * bitmap, the block bitmap, and the inode table. */
	allocated = data_block / 8;
	memset(data_bitmap, 0xff, allocated);
	/* set zeros for free blocks */
	memset(data_bitmap + allocated, 0, data_bitmap_size - allocated);
	if (nr_blocks % 8)
		data_bitmap[data_bitmap_size-1] |=
			~((1 << (nr_blocks % 8)) - 1);
	/* set the rest of bitmaps for already allocated blocks */
	data_bitmap[allocated] |= (1 << (data_block % 8)) - 1;

	/* write the inode bitmap in the file system */
	memset(buf, 0xff, BLOCK_SIZE);
	write_block(inode_table-1, buf, BLOCK_SIZE, dev);
	memset(buf, 0, nr_inodes / 8);
	buf[nr_inodes / 8] &= ~((1 << (nr_inodes % 8)) - 1);
	write_inode_bitmap(buf, dev);

	/* write the data bitmap in the file system */
	for (i = 0; i < (data_bitmap_size / BLOCK_SIZE); i++)
		write_block(D_BMAP_BLK + i, data_bitmap + (i * BLOCK_SIZE),
				BLOCK_SIZE, dev);
	if (data_bitmap_size % BLOCK_SIZE)
		write_block(D_BMAP_BLK + i, data_bitmap + (i * BLOCK_SIZE),
				data_bitmap_size % BLOCK_SIZE, dev);

	/* make the root node. root inode is always 0. */
	if (make_node(FT_ROOT, dev) != 0) {
		error("embedfs: wrong root inode");
		ret = -EFAULT;
		goto out_free_bitmap;
	}

	struct embed_inode *root_inode;

	if ((root_inode = kmalloc(sizeof(struct embed_inode))) == NULL)
		goto out_free_bitmap;

	root_inode->id = 0;
	read_inode(root_inode, dev);
	create_file("dev", FT_DIR, root_inode, dev);

	notice("block_size %d\n"
	       "free_blocks_count %d\n"
	       "the first block of inode_table %d\n"
	       "the first block of data_block %d\n"
	       "base address of device %x\n"
	       "magic %x",
	       sb->block_size,
	       sb->free_blocks_count,
	       sb->inode_table,
	       sb->data_block,
	       sb->first_block,
	       sb->magic);

	__sync(dev);
	ret = 0;

	kfree(root_inode);
out_free_bitmap:
	kfree(data_bitmap);
out_free_buf:
	kfree(buf);
out_free_sb:
	kfree(sb);
out:
	return ret;
}

int embedfs_mount(struct device *dev)
{
	unsigned int end;
	end = dev->block_size * dev->nr_blocks + dev->base_addr - 1;
	notice("embedfs addr %08x - %08x", dev->base_addr, end);

	struct embed_superblock *sb;

	if ((sb = kmalloc(sizeof(struct embed_superblock))) == NULL)
		return -ENOMEM;

	/* It doesn't need to get the locks below because nothing can hold the
	 * locks before the device is mounted. But I'm just doing double check
	 * getting the locks here. */

	/* request buffer cache */
	mutex_lock(&dev->mutex);
	if (!dev->buffer)
		dev->buffer = request_buffer(8, BLOCK_SIZE);
	mutex_unlock(&dev->mutex);

	mutex_lock(&sb_lock);
	read_superblock(sb, dev);
	mutex_unlock(&sb_lock);

	if (sb->magic != MAGIC)
		build_file_system(dev);

	mutex_lock(&sb_lock);
	read_superblock(sb, dev);
	mutex_unlock(&sb_lock);

	int err = 0;

	if (sb->magic != MAGIC)
		err = -EFAULT;
	if (sb->block_size != BLOCK_SIZE)
		err = -EFAULT;
	if (sb->first_block != dev->base_addr)
		err = -EFAULT;

	kfree(sb);

	if (err)
		error("can't build root file system");

	char *bitmap;
	if (!cached_bitmap) {
		if ((bitmap = kmalloc(BLOCK_SIZE)) == NULL)
			error("failed loading the inode bitmap");
		read_inode_bitmap(bitmap, dev);
		cached_bitmap = bitmap;
	}

	return err;
}

static struct super_operations super_ops = {
	.read_inode = embed_read_inode,
	.mount = embedfs_mount,
};

static int read_super(struct superblock *sb, struct device *dev)
{
	/* no need to read from disk because the root inode is always zero */
	sb->root_inode = 0;
	sb->op = &super_ops;

	(void)dev;
	return 0;
}

void embedfs_register()
{
	struct file_system_type *fs;

	fs = kmalloc(sizeof(struct file_system_type));
	fs->read_super = read_super;

	add_file_system(fs, "embedfs");
}
