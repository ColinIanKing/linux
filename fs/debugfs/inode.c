// SPDX-License-Identifier: GPL-2.0
/*
 *  inode.c - part of debugfs, a tiny little debug file system
 *
 *  Copyright (C) 2004,2019 Greg Kroah-Hartman <greg@kroah.com>
 *  Copyright (C) 2004 IBM Inc.
 *  Copyright (C) 2019 Linux Foundation <gregkh@linuxfoundation.org>
 *
 *  debugfs is for people to use instead of /proc or /sys.
 *  See ./Documentation/core-api/kernel-api.rst for more details.
 */

#define pr_fmt(fmt)	"debugfs: " fmt

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/debugfs.h>
#include <linux/fsnotify.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/security.h>

#include "internal.h"

#define DEBUGFS_DEFAULT_MODE	0700

static struct vfsmount *debugfs_mount;
static int debugfs_mount_count;
static bool debugfs_registered;
static unsigned int debugfs_allow __ro_after_init = DEFAULT_DEBUGFS_ALLOW_BITS;

/*
 * Don't allow access attributes to be changed whilst the kernel is locked down
 * so that we can use the file mode as part of a heuristic to determine whether
 * to lock down individual files.
 */
static int debugfs_setattr(struct mnt_idmap *idmap,
			   struct dentry *dentry, struct iattr *ia)
{
	int ret;

	if (ia->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID)) {
		ret = security_locked_down(LOCKDOWN_DEBUGFS);
		if (ret)
			return ret;
	}
	return simple_setattr(&nop_mnt_idmap, dentry, ia);
}

static const struct inode_operations debugfs_file_inode_operations = {
	.setattr	= debugfs_setattr,
};
static const struct inode_operations debugfs_dir_inode_operations = {
	.lookup		= simple_lookup,
	.setattr	= debugfs_setattr,
};
static const struct inode_operations debugfs_symlink_inode_operations = {
	.get_link	= simple_get_link,
	.setattr	= debugfs_setattr,
};

static struct inode *debugfs_get_inode(struct super_block *sb)
{
	struct inode *inode = new_inode(sb);
	if (inode) {
		inode->i_ino = get_next_ino();
		simple_inode_init_ts(inode);
	}
	return inode;
}

struct debugfs_fs_info {
	kuid_t uid;
	kgid_t gid;
	umode_t mode;
	/* Opt_* bitfield. */
	unsigned int opts;
};

enum {
	Opt_uid,
	Opt_gid,
	Opt_mode,
	Opt_source,
};

static const struct fs_parameter_spec debugfs_param_specs[] = {
	fsparam_gid	("gid",		Opt_gid),
	fsparam_u32oct	("mode",	Opt_mode),
	fsparam_uid	("uid",		Opt_uid),
	fsparam_string	("source",	Opt_source),
	{}
};

static int debugfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct debugfs_fs_info *opts = fc->s_fs_info;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, debugfs_param_specs, param, &result);
	if (opt < 0) {
		/*
                * We might like to report bad mount options here; but
                * traditionally debugfs has ignored all mount options
                */
		if (opt == -ENOPARAM)
			return 0;

		return opt;
	}

	switch (opt) {
	case Opt_uid:
		opts->uid = result.uid;
		break;
	case Opt_gid:
		opts->gid = result.gid;
		break;
	case Opt_mode:
		opts->mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_source:
		if (fc->source)
			return invalfc(fc, "Multiple sources specified");
		fc->source = param->string;
		param->string = NULL;
		break;
	/*
	 * We might like to report bad mount options here;
	 * but traditionally debugfs has ignored all mount options
	 */
	}

	opts->opts |= BIT(opt);

	return 0;
}

static void _debugfs_apply_options(struct super_block *sb, bool remount)
{
	struct debugfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode = d_inode(sb->s_root);

	/*
	 * On remount, only reset mode/uid/gid if they were provided as mount
	 * options.
	 */

	if (!remount || fsi->opts & BIT(Opt_mode)) {
		inode->i_mode &= ~S_IALLUGO;
		inode->i_mode |= fsi->mode;
	}

	if (!remount || fsi->opts & BIT(Opt_uid))
		inode->i_uid = fsi->uid;

	if (!remount || fsi->opts & BIT(Opt_gid))
		inode->i_gid = fsi->gid;
}

static void debugfs_apply_options(struct super_block *sb)
{
	_debugfs_apply_options(sb, false);
}

static void debugfs_apply_options_remount(struct super_block *sb)
{
	_debugfs_apply_options(sb, true);
}

static int debugfs_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	struct debugfs_fs_info *sb_opts = sb->s_fs_info;
	struct debugfs_fs_info *new_opts = fc->s_fs_info;

	sync_filesystem(sb);

	/* structure copy of new mount options to sb */
	*sb_opts = *new_opts;
	debugfs_apply_options_remount(sb);

	return 0;
}

static int debugfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct debugfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (!uid_eq(fsi->uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, fsi->uid));
	if (!gid_eq(fsi->gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, fsi->gid));
	if (fsi->mode != DEBUGFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mode);

	return 0;
}

static struct kmem_cache *debugfs_inode_cachep __ro_after_init;

static void init_once(void *foo)
{
	struct debugfs_inode_info *info = foo;
	inode_init_once(&info->vfs_inode);
}

static struct inode *debugfs_alloc_inode(struct super_block *sb)
{
	struct debugfs_inode_info *info;
	info = alloc_inode_sb(sb, debugfs_inode_cachep, GFP_KERNEL);
	if (!info)
		return NULL;
	return &info->vfs_inode;
}

static void debugfs_free_inode(struct inode *inode)
{
	if (S_ISLNK(inode->i_mode))
		kfree(inode->i_link);
	kmem_cache_free(debugfs_inode_cachep, DEBUGFS_I(inode));
}

static const struct super_operations debugfs_super_operations = {
	.statfs		= simple_statfs,
	.show_options	= debugfs_show_options,
	.alloc_inode	= debugfs_alloc_inode,
	.free_inode	= debugfs_free_inode,
};

static void debugfs_release_dentry(struct dentry *dentry)
{
	struct debugfs_fsdata *fsd = dentry->d_fsdata;

	if (fsd) {
		WARN_ON(!list_empty(&fsd->cancellations));
		mutex_destroy(&fsd->cancellations_mtx);
	}
	kfree(fsd);
}

static struct vfsmount *debugfs_automount(struct path *path)
{
	struct inode *inode = path->dentry->d_inode;

	return DEBUGFS_I(inode)->automount(path->dentry, inode->i_private);
}

static const struct dentry_operations debugfs_dops = {
	.d_release = debugfs_release_dentry,
	.d_automount = debugfs_automount,
};

static int debugfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	static const struct tree_descr debug_files[] = {{""}};
	int err;

	err = simple_fill_super(sb, DEBUGFS_MAGIC, debug_files);
	if (err)
		return err;

	sb->s_op = &debugfs_super_operations;
	set_default_d_op(sb, &debugfs_dops);
	sb->s_d_flags |= DCACHE_DONTCACHE;

	debugfs_apply_options(sb);

	return 0;
}

static int debugfs_get_tree(struct fs_context *fc)
{
	if (!(debugfs_allow & DEBUGFS_ALLOW_API))
		return -EPERM;

	return get_tree_single(fc, debugfs_fill_super);
}

static void debugfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations debugfs_context_ops = {
	.free		= debugfs_free_fc,
	.parse_param	= debugfs_parse_param,
	.get_tree	= debugfs_get_tree,
	.reconfigure	= debugfs_reconfigure,
};

static int debugfs_init_fs_context(struct fs_context *fc)
{
	struct debugfs_fs_info *fsi;

	fsi = kzalloc(sizeof(struct debugfs_fs_info), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	fsi->mode = DEBUGFS_DEFAULT_MODE;

	fc->s_fs_info = fsi;
	fc->ops = &debugfs_context_ops;
	return 0;
}

static struct file_system_type debug_fs_type = {
	.owner =	THIS_MODULE,
	.name =		"debugfs",
	.init_fs_context = debugfs_init_fs_context,
	.parameters =	debugfs_param_specs,
	.kill_sb =	kill_litter_super,
};
MODULE_ALIAS_FS("debugfs");

/**
 * debugfs_lookup() - look up an existing debugfs file
 * @name: a pointer to a string containing the name of the file to look up.
 * @parent: a pointer to the parent dentry of the file.
 *
 * This function will return a pointer to a dentry if it succeeds.  If the file
 * doesn't exist or an error occurs, %NULL will be returned.  The returned
 * dentry must be passed to dput() when it is no longer needed.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct dentry *debugfs_lookup(const char *name, struct dentry *parent)
{
	struct dentry *dentry;

	if (!debugfs_initialized() || IS_ERR_OR_NULL(name) || IS_ERR(parent))
		return NULL;

	if (!parent)
		parent = debugfs_mount->mnt_root;

	dentry = lookup_noperm_positive_unlocked(&QSTR(name), parent);
	if (IS_ERR(dentry))
		return NULL;
	return dentry;
}
EXPORT_SYMBOL_GPL(debugfs_lookup);

static struct dentry *start_creating(const char *name, struct dentry *parent)
{
	struct dentry *dentry;
	int error;

	if (!(debugfs_allow & DEBUGFS_ALLOW_API))
		return ERR_PTR(-EPERM);

	if (!debugfs_initialized())
		return ERR_PTR(-ENOENT);

	pr_debug("creating file '%s'\n", name);

	if (IS_ERR(parent))
		return parent;

	error = simple_pin_fs(&debug_fs_type, &debugfs_mount,
			      &debugfs_mount_count);
	if (error) {
		pr_err("Unable to pin filesystem for file '%s'\n", name);
		return ERR_PTR(error);
	}

	/* If the parent is not specified, we create it in the root.
	 * We need the root dentry to do this, which is in the super
	 * block. A pointer to that is in the struct vfsmount that we
	 * have around.
	 */
	if (!parent)
		parent = debugfs_mount->mnt_root;

	dentry = simple_start_creating(parent, name);
	if (IS_ERR(dentry)) {
		if (dentry == ERR_PTR(-EEXIST))
			pr_err("'%s' already exists in '%pd'\n", name, parent);
		simple_release_fs(&debugfs_mount, &debugfs_mount_count);
	}
	return dentry;
}

static struct dentry *failed_creating(struct dentry *dentry)
{
	inode_unlock(d_inode(dentry->d_parent));
	dput(dentry);
	simple_release_fs(&debugfs_mount, &debugfs_mount_count);
	return ERR_PTR(-ENOMEM);
}

static struct dentry *end_creating(struct dentry *dentry)
{
	inode_unlock(d_inode(dentry->d_parent));
	return dentry;
}

static struct dentry *__debugfs_create_file(const char *name, umode_t mode,
				struct dentry *parent, void *data,
				const void *aux,
				const struct file_operations *proxy_fops,
				const void *real_fops)
{
	struct dentry *dentry;
	struct inode *inode;

	if (!(mode & S_IFMT))
		mode |= S_IFREG;
	BUG_ON(!S_ISREG(mode));
	dentry = start_creating(name, parent);

	if (IS_ERR(dentry))
		return dentry;

	if (!(debugfs_allow & DEBUGFS_ALLOW_API)) {
		failed_creating(dentry);
		return ERR_PTR(-EPERM);
	}

	inode = debugfs_get_inode(dentry->d_sb);
	if (unlikely(!inode)) {
		pr_err("out of free dentries, can not create file '%s'\n",
		       name);
		return failed_creating(dentry);
	}

	inode->i_mode = mode;
	inode->i_private = data;

	inode->i_op = &debugfs_file_inode_operations;
	if (!real_fops)
		proxy_fops = &debugfs_noop_file_operations;
	inode->i_fop = proxy_fops;
	DEBUGFS_I(inode)->raw = real_fops;
	DEBUGFS_I(inode)->aux = (void *)aux;

	d_instantiate(dentry, inode);
	fsnotify_create(d_inode(dentry->d_parent), dentry);
	return end_creating(dentry);
}

struct dentry *debugfs_create_file_full(const char *name, umode_t mode,
					struct dentry *parent, void *data,
					const void *aux,
					const struct file_operations *fops)
{
	return __debugfs_create_file(name, mode, parent, data, aux,
				&debugfs_full_proxy_file_operations,
				fops);
}
EXPORT_SYMBOL_GPL(debugfs_create_file_full);

struct dentry *debugfs_create_file_short(const char *name, umode_t mode,
					struct dentry *parent, void *data,
					const void *aux,
					const struct debugfs_short_fops *fops)
{
	return __debugfs_create_file(name, mode, parent, data, aux,
				&debugfs_full_short_proxy_file_operations,
				fops);
}
EXPORT_SYMBOL_GPL(debugfs_create_file_short);

/**
 * debugfs_create_file_unsafe - create a file in the debugfs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the debugfs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fops: a pointer to a struct file_operations that should be used for
 *        this file.
 *
 * debugfs_create_file_unsafe() is completely analogous to
 * debugfs_create_file(), the only difference being that the fops
 * handed it will not get protected against file removals by the
 * debugfs core.
 *
 * It is your responsibility to protect your struct file_operation
 * methods against file removals by means of debugfs_file_get()
 * and debugfs_file_put(). ->open() is still protected by
 * debugfs though.
 *
 * Any struct file_operations defined by means of
 * DEFINE_DEBUGFS_ATTRIBUTE() is protected against file removals and
 * thus, may be used here.
 */
struct dentry *debugfs_create_file_unsafe(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops)
{

	return __debugfs_create_file(name, mode, parent, data, NULL,
				&debugfs_open_proxy_file_operations,
				fops);
}
EXPORT_SYMBOL_GPL(debugfs_create_file_unsafe);

/**
 * debugfs_create_file_size - create a file in the debugfs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the debugfs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fops: a pointer to a struct file_operations that should be used for
 *        this file.
 * @file_size: initial file size
 *
 * This is the basic "create a file" function for debugfs.  It allows for a
 * wide range of flexibility in creating a file, or a directory (if you want
 * to create a directory, the debugfs_create_dir() function is
 * recommended to be used instead.)
 */
void debugfs_create_file_size(const char *name, umode_t mode,
			      struct dentry *parent, void *data,
			      const struct file_operations *fops,
			      loff_t file_size)
{
	struct dentry *de = debugfs_create_file(name, mode, parent, data, fops);

	if (!IS_ERR(de))
		d_inode(de)->i_size = file_size;
}
EXPORT_SYMBOL_GPL(debugfs_create_file_size);

/**
 * debugfs_create_dir - create a directory in the debugfs filesystem
 * @name: a pointer to a string containing the name of the directory to
 *        create.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          directory will be created in the root of the debugfs filesystem.
 *
 * This function creates a directory in debugfs with the given name.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the debugfs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, ERR_PTR(-ERROR) will be
 * returned.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 *
 * NOTE: it's expected that most callers should _ignore_ the errors returned
 * by this function. Other debugfs functions handle the fact that the "dentry"
 * passed to them could be an error and they don't crash in that case.
 * Drivers should generally work fine even if debugfs fails to init anyway.
 */
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent)
{
	struct dentry *dentry = start_creating(name, parent);
	struct inode *inode;

	if (IS_ERR(dentry))
		return dentry;

	if (!(debugfs_allow & DEBUGFS_ALLOW_API)) {
		failed_creating(dentry);
		return ERR_PTR(-EPERM);
	}

	inode = debugfs_get_inode(dentry->d_sb);
	if (unlikely(!inode)) {
		pr_err("out of free dentries, can not create directory '%s'\n",
		       name);
		return failed_creating(dentry);
	}

	inode->i_mode = S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO;
	inode->i_op = &debugfs_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;

	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);
	d_instantiate(dentry, inode);
	inc_nlink(d_inode(dentry->d_parent));
	fsnotify_mkdir(d_inode(dentry->d_parent), dentry);
	return end_creating(dentry);
}
EXPORT_SYMBOL_GPL(debugfs_create_dir);

/**
 * debugfs_create_automount - create automount point in the debugfs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the debugfs filesystem.
 * @f: function to be called when pathname resolution steps on that one.
 * @data: opaque argument to pass to f().
 *
 * @f should return what ->d_automount() would.
 */
struct dentry *debugfs_create_automount(const char *name,
					struct dentry *parent,
					debugfs_automount_t f,
					void *data)
{
	struct dentry *dentry = start_creating(name, parent);
	struct inode *inode;

	if (IS_ERR(dentry))
		return dentry;

	if (!(debugfs_allow & DEBUGFS_ALLOW_API)) {
		failed_creating(dentry);
		return ERR_PTR(-EPERM);
	}

	inode = debugfs_get_inode(dentry->d_sb);
	if (unlikely(!inode)) {
		pr_err("out of free dentries, can not create automount '%s'\n",
		       name);
		return failed_creating(dentry);
	}

	make_empty_dir_inode(inode);
	inode->i_flags |= S_AUTOMOUNT;
	inode->i_private = data;
	DEBUGFS_I(inode)->automount = f;
	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);
	d_instantiate(dentry, inode);
	inc_nlink(d_inode(dentry->d_parent));
	fsnotify_mkdir(d_inode(dentry->d_parent), dentry);
	return end_creating(dentry);
}
EXPORT_SYMBOL(debugfs_create_automount);

/**
 * debugfs_create_symlink- create a symbolic link in the debugfs filesystem
 * @name: a pointer to a string containing the name of the symbolic link to
 *        create.
 * @parent: a pointer to the parent dentry for this symbolic link.  This
 *          should be a directory dentry if set.  If this parameter is NULL,
 *          then the symbolic link will be created in the root of the debugfs
 *          filesystem.
 * @target: a pointer to a string containing the path to the target of the
 *          symbolic link.
 *
 * This function creates a symbolic link with the given name in debugfs that
 * links to the given target path.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the debugfs_remove() function when the symbolic
 * link is to be removed (no automatic cleanup happens if your module is
 * unloaded, you are responsible here.)  If an error occurs, ERR_PTR(-ERROR)
 * will be returned.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct dentry *debugfs_create_symlink(const char *name, struct dentry *parent,
				      const char *target)
{
	struct dentry *dentry;
	struct inode *inode;
	char *link = kstrdup(target, GFP_KERNEL);
	if (!link)
		return ERR_PTR(-ENOMEM);

	dentry = start_creating(name, parent);
	if (IS_ERR(dentry)) {
		kfree(link);
		return dentry;
	}

	inode = debugfs_get_inode(dentry->d_sb);
	if (unlikely(!inode)) {
		pr_err("out of free dentries, can not create symlink '%s'\n",
		       name);
		kfree(link);
		return failed_creating(dentry);
	}
	inode->i_mode = S_IFLNK | S_IRWXUGO;
	inode->i_op = &debugfs_symlink_inode_operations;
	inode->i_link = link;
	d_instantiate(dentry, inode);
	return end_creating(dentry);
}
EXPORT_SYMBOL_GPL(debugfs_create_symlink);

static void __debugfs_file_removed(struct dentry *dentry)
{
	struct debugfs_fsdata *fsd;

	/*
	 * Paired with the closing smp_mb() implied by a successful
	 * cmpxchg() in debugfs_file_get(): either
	 * debugfs_file_get() must see a dead dentry or we must see a
	 * debugfs_fsdata instance at ->d_fsdata here (or both).
	 */
	smp_mb();
	fsd = READ_ONCE(dentry->d_fsdata);
	if (!fsd)
		return;

	/* if this was the last reference, we're done */
	if (refcount_dec_and_test(&fsd->active_users))
		return;

	/*
	 * If there's still a reference, the code that obtained it can
	 * be in different states:
	 *  - The common case of not using cancellations, or already
	 *    after debugfs_leave_cancellation(), where we just need
	 *    to wait for debugfs_file_put() which signals the completion;
	 *  - inside a cancellation section, i.e. between
	 *    debugfs_enter_cancellation() and debugfs_leave_cancellation(),
	 *    in which case we need to trigger the ->cancel() function,
	 *    and then wait for debugfs_file_put() just like in the
	 *    previous case;
	 *  - before debugfs_enter_cancellation() (but obviously after
	 *    debugfs_file_get()), in which case we may not see the
	 *    cancellation in the list on the first round of the loop,
	 *    but debugfs_enter_cancellation() signals the completion
	 *    after adding it, so this code gets woken up to call the
	 *    ->cancel() function.
	 */
	while (refcount_read(&fsd->active_users)) {
		struct debugfs_cancellation *c;

		/*
		 * Lock the cancellations. Note that the cancellations
		 * structs are meant to be on the stack, so we need to
		 * ensure we either use them here or don't touch them,
		 * and debugfs_leave_cancellation() will wait for this
		 * to be finished processing before exiting one. It may
		 * of course win and remove the cancellation, but then
		 * chances are we never even got into this bit, we only
		 * do if the refcount isn't zero already.
		 */
		mutex_lock(&fsd->cancellations_mtx);
		while ((c = list_first_entry_or_null(&fsd->cancellations,
						     typeof(*c), list))) {
			list_del_init(&c->list);
			c->cancel(dentry, c->cancel_data);
		}
		mutex_unlock(&fsd->cancellations_mtx);

		wait_for_completion(&fsd->active_users_drained);
	}
}

static void remove_one(struct dentry *victim)
{
        if (d_is_reg(victim))
		__debugfs_file_removed(victim);
	simple_release_fs(&debugfs_mount, &debugfs_mount_count);
}

/**
 * debugfs_remove - recursively removes a directory
 * @dentry: a pointer to a the dentry of the directory to be removed.  If this
 *          parameter is NULL or an error value, nothing will be done.
 *
 * This function recursively removes a directory tree in debugfs that
 * was previously created with a call to another debugfs function
 * (like debugfs_create_file() or variants thereof.)
 *
 * This function is required to be called in order for the file to be
 * removed, no automatic cleanup of files will happen when a module is
 * removed, you are responsible here.
 */
void debugfs_remove(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry))
		return;

	simple_pin_fs(&debug_fs_type, &debugfs_mount, &debugfs_mount_count);
	simple_recursive_removal(dentry, remove_one);
	simple_release_fs(&debugfs_mount, &debugfs_mount_count);
}
EXPORT_SYMBOL_GPL(debugfs_remove);

/**
 * debugfs_lookup_and_remove - lookup a directory or file and recursively remove it
 * @name: a pointer to a string containing the name of the item to look up.
 * @parent: a pointer to the parent dentry of the item.
 *
 * This is the equlivant of doing something like
 * debugfs_remove(debugfs_lookup(..)) but with the proper reference counting
 * handled for the directory being looked up.
 */
void debugfs_lookup_and_remove(const char *name, struct dentry *parent)
{
	struct dentry *dentry;

	dentry = debugfs_lookup(name, parent);
	if (!dentry)
		return;

	debugfs_remove(dentry);
	dput(dentry);
}
EXPORT_SYMBOL_GPL(debugfs_lookup_and_remove);

/**
 * debugfs_change_name - rename a file/directory in the debugfs filesystem
 * @dentry: dentry of an object to be renamed.
 * @fmt: format for new name
 *
 * This function renames a file/directory in debugfs.  The target must not
 * exist for rename to succeed.
 *
 * This function will return 0 on success and -E... on failure.
 *
 * If debugfs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
int __printf(2, 3) debugfs_change_name(struct dentry *dentry, const char *fmt, ...)
{
	int error = 0;
	const char *new_name;
	struct name_snapshot old_name;
	struct dentry *parent, *target;
	struct inode *dir;
	va_list ap;

	if (IS_ERR_OR_NULL(dentry))
		return 0;

	va_start(ap, fmt);
	new_name = kvasprintf_const(GFP_KERNEL, fmt, ap);
	va_end(ap);
	if (!new_name)
		return -ENOMEM;

	parent = dget_parent(dentry);
	dir = d_inode(parent);
	inode_lock(dir);

	take_dentry_name_snapshot(&old_name, dentry);

	if (WARN_ON_ONCE(dentry->d_parent != parent)) {
		error = -EINVAL;
		goto out;
	}
	if (strcmp(old_name.name.name, new_name) == 0)
		goto out;
	target = lookup_noperm(&QSTR(new_name), parent);
	if (IS_ERR(target)) {
		error = PTR_ERR(target);
		goto out;
	}
	if (d_really_is_positive(target)) {
		dput(target);
		error = -EINVAL;
		goto out;
	}
	simple_rename_timestamp(dir, dentry, dir, target);
	d_move(dentry, target);
	dput(target);
	fsnotify_move(dir, dir, &old_name.name, d_is_dir(dentry), NULL, dentry);
out:
	release_dentry_name_snapshot(&old_name);
	inode_unlock(dir);
	dput(parent);
	kfree_const(new_name);
	return error;
}
EXPORT_SYMBOL_GPL(debugfs_change_name);

/**
 * debugfs_initialized - Tells whether debugfs has been registered
 */
bool debugfs_initialized(void)
{
	return debugfs_registered;
}
EXPORT_SYMBOL_GPL(debugfs_initialized);

static int __init debugfs_kernel(char *str)
{
	if (str) {
		if (!strcmp(str, "on"))
			debugfs_allow = DEBUGFS_ALLOW_API | DEBUGFS_ALLOW_MOUNT;
		else if (!strcmp(str, "no-mount"))
			debugfs_allow = DEBUGFS_ALLOW_API;
		else if (!strcmp(str, "off"))
			debugfs_allow = 0;
	}

	return 0;
}
early_param("debugfs", debugfs_kernel);
static int __init debugfs_init(void)
{
	int retval;

	if (!(debugfs_allow & DEBUGFS_ALLOW_MOUNT))
		return -EPERM;

	retval = sysfs_create_mount_point(kernel_kobj, "debug");
	if (retval)
		return retval;

	debugfs_inode_cachep = kmem_cache_create("debugfs_inode_cache",
				sizeof(struct debugfs_inode_info), 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
				init_once);
	if (debugfs_inode_cachep == NULL) {
		sysfs_remove_mount_point(kernel_kobj, "debug");
		return -ENOMEM;
	}

	retval = register_filesystem(&debug_fs_type);
	if (retval) { // Really not going to happen
		sysfs_remove_mount_point(kernel_kobj, "debug");
		kmem_cache_destroy(debugfs_inode_cachep);
		return retval;
	}
	debugfs_registered = true;
	return 0;
}
core_initcall(debugfs_init);
