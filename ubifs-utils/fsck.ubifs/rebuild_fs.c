// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Huawei Technologies Co, Ltd.
 *
 * Authors: Zhihao Cheng <chengzhihao1@huawei.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/stat.h>

#include "linux_err.h"
#include "bitops.h"
#include "kmem.h"
#include "ubifs.h"
#include "defs.h"
#include "debug.h"
#include "key.h"
#include "misc.h"
#include "fsck.ubifs.h"

/**
 * scanned_info - nodes and files information from scanning.
 * @valid_inos: the tree of scanned inode nodes with 'nlink > 0'
 * @del_inos: the tree of scanned inode nodes with 'nlink = 0'
 * @valid_dents: the tree of scanned dentry nodes with 'inum > 0'
 * @del_dents: the tree of scanned dentry nodes with 'inum = 0'
 */
struct scanned_info {
	struct rb_root valid_inos;
	struct rb_root del_inos;
	struct rb_root valid_dents;
	struct rb_root del_dents;
};

static int init_rebuild_info(struct ubifs_info *c)
{
	int err;

	c->sbuf = vmalloc(c->leb_size);
	if (!c->sbuf) {
		log_err(c, errno, "can not allocate sbuf");
		return -ENOMEM;
	}
	FSCK(c)->rebuild = kzalloc(sizeof(struct ubifs_rebuild_info),
				   GFP_KERNEL);
	if (!FSCK(c)->rebuild) {
		err = -ENOMEM;
		log_err(c, errno, "can not allocate rebuild info");
		goto out;
	}
	FSCK(c)->rebuild->scanned_files = RB_ROOT;

	return 0;

out:
	vfree(c->sbuf);
	return err;
}

static void destroy_rebuild_info(struct ubifs_info *c)
{
	kfree(FSCK(c)->rebuild);
	vfree(c->sbuf);
}

/**
 * insert_or_update_ino_node - insert or update inode node.
 * @c: UBIFS file-system description object
 * @new_ino: new inode node
 * @tree: a tree to record valid/deleted inode node info
 *
 * This function inserts @new_ino into the @tree, or updates inode node
 * if it already exists in the tree. Returns zero in case of success, a
 * negative error code in case of failure.
 */
static int insert_or_update_ino_node(struct ubifs_info *c,
				     struct scanned_ino_node *new_ino,
				     struct rb_root *tree)
{
	int cmp;
	struct scanned_ino_node *ino_node, *old_ino_node = NULL;
	struct rb_node **p, *parent = NULL;

	p = &tree->rb_node;
	while (*p) {
		parent = *p;
		ino_node = rb_entry(parent, struct scanned_ino_node, rb);
		cmp = keys_cmp(c, &new_ino->key, &ino_node->key);
		if (cmp < 0) {
			p = &(*p)->rb_left;
		} else if (cmp > 0) {
			p = &(*p)->rb_right;
		} else {
			old_ino_node = ino_node;
			break;
		}
	}
	if (old_ino_node) {
		if (old_ino_node->header.sqnum < new_ino->header.sqnum) {
			size_t len = offsetof(struct scanned_ino_node, rb);

			memcpy(old_ino_node, new_ino, len);
		}
		return 0;
	}

	ino_node = kmalloc(sizeof(struct scanned_ino_node), GFP_KERNEL);
	if (!ino_node)
		return -ENOMEM;

	*ino_node = *new_ino;
	rb_link_node(&ino_node->rb, parent, p);
	rb_insert_color(&ino_node->rb, tree);

	return 0;
}

/**
 * insert_or_update_dent_node - insert or update dentry node.
 * @c: UBIFS file-system description object
 * @new_dent: new dentry node
 * @tree: a tree to record valid/deleted dentry node info
 *
 * This function inserts @new_dent into the @tree, or updates dent node
 * if it already exists in the tree. Returns zero in case of success, a
 * negative error code in case of failure.
 */
static int insert_or_update_dent_node(struct ubifs_info *c,
				      struct scanned_dent_node *new_dent,
				      struct rb_root *tree)
{
	int cmp, nlen;
	struct scanned_dent_node *dent_node, *old_dent_node = NULL;
	struct rb_node **p, *parent = NULL;

	p = &tree->rb_node;
	while (*p) {
		parent = *p;
		dent_node = rb_entry(parent, struct scanned_dent_node, rb);
		cmp = keys_cmp(c, &new_dent->key, &dent_node->key);
		if (cmp < 0) {
			p = &(*p)->rb_left;
		} else if (cmp > 0) {
			p = &(*p)->rb_right;
		} else {
			nlen = min(new_dent->nlen, dent_node->nlen);
			cmp = strncmp(new_dent->name, dent_node->name, nlen) ? :
				      new_dent->nlen - dent_node->nlen;
			if (cmp < 0) {
				p = &(*p)->rb_left;
			} else if (cmp > 0) {
				p = &(*p)->rb_right;
			} else {
				old_dent_node = dent_node;
				break;
			}
		}
	}
	if (old_dent_node) {
		if (old_dent_node->header.sqnum < new_dent->header.sqnum) {
			size_t len = offsetof(struct scanned_dent_node, rb);

			memcpy(old_dent_node, new_dent, len);
		}
		return 0;
	}

	dent_node = kmalloc(sizeof(struct scanned_dent_node), GFP_KERNEL);
	if (!dent_node)
		return -ENOMEM;

	*dent_node = *new_dent;
	rb_link_node(&dent_node->rb, parent, p);
	rb_insert_color(&dent_node->rb, tree);

	return 0;
}

/**
 * process_scanned_node - process scanned node.
 * @c: UBIFS file-system description object
 * @lnum: logical eraseblock number
 * @snod: scanned node
 * @si: records nodes and files information during scanning
 *
 * This function parses, checks and records scanned node information.
 * Returns zero in case of success, 1% if the scanned LEB doesn't hold file
 * data and should be ignored(eg. index LEB), a negative error code in case
 * of failure.
 */
static int process_scanned_node(struct ubifs_info *c, int lnum,
				struct ubifs_scan_node *snod,
				struct scanned_info *si)
{
	ino_t inum;
	int offs = snod->offs;
	void *node = snod->node;
	union ubifs_key *key = &snod->key;
	struct rb_root *tree;
	struct scanned_node *sn;
	struct scanned_ino_node ino_node;
	struct scanned_dent_node dent_node;
	struct scanned_data_node data_node;
	struct scanned_trun_node trun_node;

	switch (snod->type) {
	case UBIFS_INO_NODE:
	{
		if (!parse_ino_node(c, lnum, offs, node, key, &ino_node))
			return 0;

		tree = &si->del_inos;
		if (ino_node.nlink)
			tree = &si->valid_inos;
		return insert_or_update_ino_node(c, &ino_node, tree);
	}
	case UBIFS_DENT_NODE:
	case UBIFS_XENT_NODE:
	{
		if (!parse_dent_node(c, lnum, offs, node, key, &dent_node))
			return 0;

		tree = &si->del_dents;
		if (dent_node.inum)
			tree = &si->valid_dents;
		return insert_or_update_dent_node(c, &dent_node, tree);
	}
	case UBIFS_DATA_NODE:
	{
		if (!parse_data_node(c, lnum, offs, node, key, &data_node))
			return 0;

		inum = key_inum(c, key);
		sn = (struct scanned_node *)&data_node;
		break;
	}
	case UBIFS_TRUN_NODE:
	{
		if (!parse_trun_node(c, lnum, offs, node, key, &trun_node))
			return 0;

		inum = le32_to_cpu(((struct ubifs_trun_node *)node)->inum);
		sn = (struct scanned_node *)&trun_node;
		break;
	}
	default:
		dbg_fsck("skip node type %d, at %d:%d, in %s",
			 snod->type, lnum, offs, c->dev_name);
		return 1;
	}

	tree = &FSCK(c)->rebuild->scanned_files;
	return insert_or_update_file(c, tree, sn, key_type(c, key), inum);
}

/**
 * destroy_scanned_info - destroy scanned nodes.
 * @c: UBIFS file-system description object
 * @si: records nodes and files information during scanning
 *
 * Destroy scanned files and all data/dentry nodes attached to file, destroy
 * valid/deleted inode/dentry info.
 */
static void destroy_scanned_info(struct ubifs_info *c, struct scanned_info *si)
{
	struct scanned_ino_node *ino_node;
	struct scanned_dent_node *dent_node;
	struct rb_node *this;

	destroy_file_tree(c, &FSCK(c)->rebuild->scanned_files);

	this = rb_first(&si->valid_inos);
	while (this) {
		ino_node = rb_entry(this, struct scanned_ino_node, rb);
		this = rb_next(this);

		rb_erase(&ino_node->rb, &si->valid_inos);
		kfree(ino_node);
	}

	this = rb_first(&si->del_inos);
	while (this) {
		ino_node = rb_entry(this, struct scanned_ino_node, rb);
		this = rb_next(this);

		rb_erase(&ino_node->rb, &si->del_inos);
		kfree(ino_node);
	}

	this = rb_first(&si->valid_dents);
	while (this) {
		dent_node = rb_entry(this, struct scanned_dent_node, rb);
		this = rb_next(this);

		rb_erase(&dent_node->rb, &si->valid_dents);
		kfree(dent_node);
	}

	this = rb_first(&si->del_dents);
	while (this) {
		dent_node = rb_entry(this, struct scanned_dent_node, rb);
		this = rb_next(this);

		rb_erase(&dent_node->rb, &si->del_dents);
		kfree(dent_node);
	}
}

/**
 * scan_nodes - scan node information from flash.
 * @c: UBIFS file-system description object
 * @si: records nodes and files information during scanning
 *
 * This function scans nodes from flash, all ino/dent nodes are split
 * into valid tree and deleted tree, all trun/data nodes are collected
 * into file, the file is inserted into @FSCK(c)->rebuild->scanned_files.
 */
static int scan_nodes(struct ubifs_info *c, struct scanned_info *si)
{
	int lnum, err = 0;
	struct ubifs_scan_leb *sleb;
	struct ubifs_scan_node *snod;

	for (lnum = c->main_first; lnum < c->leb_cnt; ++lnum) {
		dbg_fsck("scan nodes at LEB %d, in %s", lnum, c->dev_name);

		sleb = ubifs_scan(c, lnum, 0, c->sbuf, 1);
		if (IS_ERR(sleb)) {
			if (PTR_ERR(sleb) != -EUCLEAN)
				return PTR_ERR(sleb);

			sleb = ubifs_recover_leb(c, lnum, 0, c->sbuf, -1);
			if (IS_ERR(sleb)) {
				if (PTR_ERR(sleb) != -EUCLEAN)
					return PTR_ERR(sleb);

				/* This LEB holds corrupted data, abandon it. */
				continue;
			}
		}

		list_for_each_entry(snod, &sleb->nodes, list) {
			if (snod->sqnum > c->max_sqnum)
				c->max_sqnum = snod->sqnum;

			err = process_scanned_node(c, lnum, snod, si);
			if (err < 0) {
				log_err(c, 0, "process node failed at LEB %d, err %d",
					lnum, err);
				ubifs_scan_destroy(sleb);
				goto out;
			} else if (err == 1) {
				err = 0;
				break;
			}
		}

		ubifs_scan_destroy(sleb);
	}

out:
	return err;
}

/**
 * ubifs_rebuild_filesystem - Rebuild filesystem.
 * @c: UBIFS file-system description object
 *
 * Scanning nodes from UBI volume and rebuild filesystem. Any inconsistent
 * problems or corrupted data will be fixed.
 */
int ubifs_rebuild_filesystem(struct ubifs_info *c)
{
	int err = 0;
	struct scanned_info si;

	si.valid_inos = si.del_inos = si.valid_dents = si.del_dents = RB_ROOT;
	log_out(c, "Start rebuilding filesystem (Notice: dropping data/recovering deleted data can't be awared)");
	FSCK(c)->mode = REBUILD_MODE;

	err = init_rebuild_info(c);
	if (err) {
		exit_code |= FSCK_ERROR;
		return err;
	}

	/* Step 1: Scan valid/deleted nodes from volume. */
	log_out(c, "Scan nodes");
	err = scan_nodes(c, &si);
	if (err)
		exit_code |= FSCK_ERROR;

	destroy_scanned_info(c, &si);
	destroy_rebuild_info(c);

	return err;
}
