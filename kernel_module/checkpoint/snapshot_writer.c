/* SPDX-License-Identifier: GPL-2.0 */
#include <crypto/hash.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "snapshot_writer.h"

static int write_all(struct file *file, loff_t *pos, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len) {
		n = kernel_write(file, p, len, pos);
		if (n < 0)
			return n;
		if (!n)
			return -EIO;
		p += n;
		len -= n;
	}
	return 0;
}

static int unlink_path(const char *path)
{
	struct path parent;
	struct dentry *dentry;
	char *slash, *name, *copy;
	int ret;

	copy = kstrdup(path, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	slash = strrchr(copy, '/');
	if (!slash) {
		kfree(copy);
		return -EINVAL;
	}
	name = slash + 1;
	*slash = '\0';
	ret = kern_path(*copy ? copy : "/", LOOKUP_PARENT, &parent);
	if (ret)
		goto out;
	inode_lock(parent.dentry->d_inode);
	dentry = lookup_one_len(name, parent.dentry, strlen(name));
	if (IS_ERR(dentry)) {
		ret = PTR_ERR(dentry);
		inode_unlock(parent.dentry->d_inode);
		path_put(&parent);
		goto out;
	}
	if (d_really_is_positive(dentry)) {
		ret = vfs_unlink(parent.dentry->d_inode, dentry, NULL);
	}
	inode_unlock(parent.dentry->d_inode);
	dput(dentry);
	path_put(&parent);
out:
	kfree(copy);
	return ret;
}

static int rename_atomic(const char *old_name, const char *new_name)
{
	struct path old_parent, new_parent;
	struct dentry *old_dentry, *new_dentry;
	char *old_copy, *new_copy, *slash;
	char *old_base, *new_base;
	int ret;

	old_copy = kstrdup(old_name, GFP_KERNEL);
	new_copy = kstrdup(new_name, GFP_KERNEL);
	if (!old_copy || !new_copy) {
		kfree(old_copy);
		kfree(new_copy);
		return -ENOMEM;
	}
	slash = strrchr(old_copy, '/');
	if (!slash) { ret = -EINVAL; goto out_free; }
	old_base = slash + 1; *slash = '\0';
	slash = strrchr(new_copy, '/');
	if (!slash) { ret = -EINVAL; goto out_free; }
	new_base = slash + 1; *slash = '\0';
	ret = kern_path(*old_copy ? old_copy : "/", LOOKUP_PARENT, &old_parent);
	if (ret) goto out_free;
	ret = kern_path(*new_copy ? new_copy : "/", LOOKUP_PARENT, &new_parent);
	if (ret) { path_put(&old_parent); goto out_free; }
	old_dentry = lookup_one_len_unlocked(old_base, old_parent.dentry,
					     strlen(old_base));
	new_dentry = lookup_one_len_unlocked(new_base, new_parent.dentry,
					     strlen(new_base));
	if (IS_ERR(old_dentry) || IS_ERR(new_dentry)) {
		ret = -ENOENT;
		if (!IS_ERR(old_dentry)) dput(old_dentry);
		if (!IS_ERR(new_dentry)) dput(new_dentry);
		path_put(&new_parent); path_put(&old_parent); goto out_free;
	}
	lock_rename(old_parent.dentry, new_parent.dentry);
	ret = vfs_rename(old_parent.dentry->d_inode, old_dentry,
			 new_parent.dentry->d_inode, new_dentry, NULL, 0);
	unlock_rename(old_parent.dentry, new_parent.dentry);
	dput(old_dentry); dput(new_dentry);
	path_put(&new_parent); path_put(&old_parent);
out_free:
	kfree(old_copy); kfree(new_copy);
	return ret;
}

static int sha256_file(struct file *file, u64 *digest)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	char *buf;
	loff_t pos = 0;
	ssize_t n;
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) return PTR_ERR(tfm);
	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!desc || !buf) { ret = -ENOMEM; goto out; }
	desc->tfm = tfm;
	ret = crypto_shash_init(desc);
	if (ret) goto out;
	while (pos < i_size_read(file_inode(file))) {
		n = kernel_read(file, buf, PAGE_SIZE, &pos);
		if (n < 0) { ret = n; goto out; }
		if (!n) { ret = -EIO; goto out; }
		/* checksum field is already zero while hashing. */
		ret = crypto_shash_update(desc, buf, n);
		if (ret) goto out;
	}
	{
		u8 out[32];
		ret = crypto_shash_final(desc, out);
		if (!ret)
			memcpy(digest, out, sizeof(*digest));
	}
out:
	kfree(buf); kfree(desc); crypto_free_shash(tfm);
	return ret;
}

int criu_snapshot_writer_open(struct criu_snapshot_writer *w,
			      const char *path,
			      const struct criu_snapshot_header *header)
{
	struct criu_snapshot_header h;
	if (!w || !path || !header || header->header_size != CRIU_SNAPSHOT_HEADER_SIZE)
		return -EINVAL;
	memset(w, 0, sizeof(*w));
	w->path = kstrdup(path, GFP_KERNEL);
	w->tmp_path = kasprintf(GFP_KERNEL, "%s.tmp", path);
	if (!w->path || !w->tmp_path) { criu_snapshot_writer_abort(w); return -ENOMEM; }
	unlink_path(w->tmp_path);
	w->file = filp_open(w->tmp_path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (IS_ERR(w->file)) { int ret = PTR_ERR(w->file); w->file = NULL; criu_snapshot_writer_abort(w); return ret; }
	h = *header;
	h.checksum = 0;
	h.total_size = 0;
	h.record_count = 0;
	if (write_all(w->file, &w->pos, &h, sizeof(h))) { criu_snapshot_writer_abort(w); return -EIO; }
	w->header = h;
	w->total_size = sizeof(h);
	return 0;
}

static int criu_snapshot_writer_record_flags(struct criu_snapshot_writer *w,
					     u16 type, u16 flags,
					     const void *payload, u64 length)
{
	struct criu_snapshot_tlv tlv;
	if (!w || !w->file || w->ended || (!payload && length) ||
	    (flags & ~CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE) ||
	    length > CRIU_SNAPSHOT_MAX_RECORD_SIZE ||
	    w->record_count >= CRIU_SNAPSHOT_MAX_RECORDS ||
	    w->total_size > CRIU_SNAPSHOT_MAX_TOTAL_SIZE - sizeof(tlv) - length)
		return -EINVAL;
	tlv.type = type; tlv.flags = flags; tlv.reserved = 0; tlv.length = length;
	if (write_all(w->file, &w->pos, &tlv, sizeof(tlv)) ||
	    write_all(w->file, &w->pos, payload, length))
		return -EIO;
	w->total_size += sizeof(tlv) + length;
	w->record_count++;
	if (type == CRIU_SNAPSHOT_REC_END) w->ended = true;
	return 0;
}

int criu_snapshot_writer_record(struct criu_snapshot_writer *w, u16 type,
				u16 flags, const void *payload, u64 length)
{
	if (flags)
		return -EINVAL;
	if (w && w->process_owner_pid)
		return criu_snapshot_writer_process_record(
			w, w->process_owner_pid, type, flags, payload, length);
	return criu_snapshot_writer_record_flags(w, type, flags, payload, length);
}

int criu_snapshot_writer_global_record(struct criu_snapshot_writer *w,
				       u16 type, const void *payload,
				       u64 length)
{
	return criu_snapshot_writer_record_flags(w, type, 0, payload, length);
}

int criu_snapshot_writer_process_record(struct criu_snapshot_writer *w,
					u32 owner_pid, u16 type, u16 flags,
					const void *payload, u64 length)
{
	struct criu_snapshot_process_scope *scope_payload;
	u64 total;
	int ret;

	if (!w || !owner_pid || flags || (!payload && length) ||
	    length > CRIU_SNAPSHOT_MAX_RECORD_SIZE -
		     CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE)
		return -EINVAL;
	total = CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE + length;
	scope_payload = kmalloc(total, GFP_KERNEL);
	if (!scope_payload)
		return -ENOMEM;
	scope_payload->owner_pid = cpu_to_le32(owner_pid);
	scope_payload->reserved = 0;
	if (length)
		memcpy((u8 *)scope_payload + CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE,
		       payload, length);
	ret = criu_snapshot_writer_record_flags(
		w, type, CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE,
		scope_payload, total);
	kfree(scope_payload);
	return ret;
}

void criu_snapshot_writer_set_process_owner(
	struct criu_snapshot_writer *writer, u32 owner_pid)
{
	if (writer)
		writer->process_owner_pid = owner_pid;
}

int criu_snapshot_writer_finish(struct criu_snapshot_writer *w)
{
	struct criu_snapshot_header h;
	struct criu_snapshot_footer f;
	u64 digest = 0;
	loff_t pos;
	int ret;
	if (!w || !w->file || !w->ended ||
	    w->total_size > CRIU_SNAPSHOT_MAX_TOTAL_SIZE - CRIU_SNAPSHOT_FOOTER_SIZE)
		return -EINVAL;
	pr_info("criu_writer: finish begin path=%s size=%llu\n", w->path,
		(unsigned long long)w->total_size);
	h = w->header;
	h.record_count = w->record_count;
	h.total_size = w->total_size + sizeof(f);
	h.checksum = 0;
	pos = 0;
	ret = write_all(w->file, &pos, &h, sizeof(h));
	if (ret) return ret;
	ret = sha256_file(w->file, &digest);
	pr_info("criu_writer: hash ret=%d\n", ret);
	if (ret) return ret;
	h.checksum = digest;
	pos = offsetof(struct criu_snapshot_header, checksum);
	ret = write_all(w->file, &pos, &digest, sizeof(digest));
	if (ret) return ret;
	f.magic = CRIU_SNAPSHOT_MAGIC; f.version = CRIU_SNAPSHOT_VERSION;
	f.record_count = w->record_count; f.checksum = digest;
	pos = w->total_size;
	ret = write_all(w->file, &pos, &f, sizeof(f));
	if (ret) return ret;
	vfs_fsync(w->file, 0);
	pr_info("criu_writer: fsync done\n");
	filp_close(w->file, NULL); w->file = NULL;
	pr_info("criu_writer: close done, renaming\n");
	ret = rename_atomic(w->tmp_path, w->path);
	pr_info("criu_writer: rename ret=%d\n", ret);
	if (ret) { unlink_path(w->tmp_path); return ret; }
	kfree(w->tmp_path); kfree(w->path); w->tmp_path = NULL; w->path = NULL;
	return 0;
}

void criu_snapshot_writer_abort(struct criu_snapshot_writer *w)
{
	if (!w) return;
	if (w->file && !IS_ERR(w->file)) filp_close(w->file, NULL);
	if (w->tmp_path) unlink_path(w->tmp_path);
	kfree(w->tmp_path); kfree(w->path);
	memset(w, 0, sizeof(*w));
}
