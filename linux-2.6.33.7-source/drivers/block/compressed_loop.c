/*
 *  compressed_loop.c: Read-only compressed loop blockdevice
 *  hacked up by Rusty in 1999, extended and maintained by Klaus Knopper
 *
 *  A cloop file looks like this:
 *  [32-bit uncompressed block size: network order]
 *  [32-bit number of blocks (n_blocks): network order]
 *  [64-bit file offsets of start of blocks: network order]
 *    ...
 *    (n_blocks + 1).
 * n_blocks consisting of:
 *   [compressed block]
 *
 * Every version greatly inspired by code seen in loop.c
 * by Theodore Ts'o, 3/29/93.
 *
 * Copyright 1999-2009 by Paul `Rusty' Russell & Klaus Knopper.
 * Redistribution of this file is permitted under the GNU Public License.
 *
 */

#define CLOOP_NAME "cloop"
#define CLOOP_VERSION "2.631"
#define CLOOP_MAX 8

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME cloop
#endif

#ifndef KBUILD_BASENAME
#define KBUILD_BASENAME cloop
#endif

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <asm/div64.h> /* do_div() for 64bit division */
#include <asm/uaccess.h>
#include <asm/byteorder.h>
/* Use zlib_inflate from lib/zlib_inflate */
#include <linux/zutil.h>
#include <linux/loop.h>
#include <linux/kthread.h>
#include "compressed_loop.h"

/* New License scheme */
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
#ifdef MODULE_AUTHOR
MODULE_AUTHOR("Klaus Knopper (current maintainer), Paul Russel (initial Kernel 2.2 version)");
#endif
#ifdef MODULE_DESCRIPTION
MODULE_DESCRIPTION("Transparently decompressing loopback block device");
#endif

#ifndef MIN
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#endif

/* Use experimental major for now */
#define MAJOR_NR 240

/* #define DEVICE_NAME CLOOP_NAME */
/* #define DEVICE_NR(device) (MINOR(device)) */
/* #define DEVICE_ON(device) */
/* #define DEVICE_OFF(device) */
/* #define DEVICE_NO_RANDOM */
/* #define TIMEOUT_VALUE (6 * HZ) */

#include <linux/blkdev.h>
#include <linux/buffer_head.h>

#if 0
#define DEBUGP printk
#else
#define DEBUGP(format, x...)
#endif

/* One file can be opened at module insertion time */
/* insmod cloop file=/path/to/file */
static char *file=NULL;
static unsigned int preload=0;
module_param(file, charp, 0);
module_param(preload, uint, 0);
MODULE_PARM_DESC(file, "Initial cloop image file (full path) for /dev/cloop");
MODULE_PARM_DESC(preload, "Preload n blocks of cloop data into memory");

static struct file *initial_file=NULL;
static int cloop_major=MAJOR_NR;

/* Number of buffered decompressed blocks */
#define BUFFERED_BLOCKS 8
struct cloop_device
{
 /* Copied straight from the file */
 struct cloop_head head;

 /* An array of offsets of compressed blocks within the file */
 loff_t *offsets;

 /* We buffer some uncompressed blocks for performance */
 int buffered_blocknum[BUFFERED_BLOCKS];
 int current_bufnum;
 void *buffer[BUFFERED_BLOCKS];
 void *compressed_buffer;
 size_t preload_array_size; /* Size of pointer array in blocks */
 size_t preload_size;       /* Number of successfully allocated blocks */
 char **preload_cache;      /* Pointers to preloaded blocks */

 z_stream zstream;

 struct file   *backing_file;  /* associated file */
 struct inode  *backing_inode; /* for bmap */

 unsigned long largest_block;
 unsigned int underlying_blksize;
 int clo_number;
 int refcnt;
 struct block_device *bdev;
 int isblkdev;
 /* Lock for kernel block device queue */
 spinlock_t queue_lock;
 struct list_head clo_list;
 struct task_struct *clo_thread;
 wait_queue_head_t clo_event;
 struct request_queue *clo_queue;
 struct gendisk *clo_disk;
 int suspended;
 char clo_file_name[LO_NAME_SIZE];
};

static struct cloop_device cloop_dev[CLOOP_MAX];
static const char *cloop_name=CLOOP_NAME;
static const int cloop_max = CLOOP_MAX;
static int cloop_count = 0;

#if (!(defined(CONFIG_ZLIB_INFLATE) || defined(CONFIG_ZLIB_INFLATE_MODULE))) /* Must be compiled into kernel. */
#error  "Invalid Kernel configuration. CONFIG_ZLIB_INFLATE support is needed for cloop."
#endif

/* Use __get_free_pages instead of vmalloc, allows up to 32 pages,
 * 2MB in one piece */
static void *cloop_malloc(size_t size)
{
 int order = get_order(size);
 if(order <= KMALLOC_MAX_ORDER)
   return (void *)kmalloc(size, GFP_KERNEL);
 else if(order < MAX_ORDER)
   return (void *)__get_free_pages(GFP_KERNEL, order);
 return (void *)vmalloc(size);
}

static void cloop_free(void *mem, size_t size)
{
 int order = get_order(size);
 if(order <= KMALLOC_MAX_ORDER)
   kfree(mem);
 else if(order < MAX_ORDER)
   free_pages((unsigned long)mem, order);
 else vfree(mem);
}

static int uncompress(struct cloop_device *clo,
                      unsigned char *dest, unsigned long *destLen,
                      unsigned char *source, unsigned long sourceLen)
{
 /* Most of this code can be found in fs/cramfs/uncompress.c */
 int err;
 clo->zstream.next_in = source;
 clo->zstream.avail_in = sourceLen;
 clo->zstream.next_out = dest;
 clo->zstream.avail_out = *destLen;
 err = zlib_inflateReset(&clo->zstream);
 if (err != Z_OK)
  {
   printk(KERN_ERR "%s: zlib_inflateReset error %d\n", cloop_name, err);
   zlib_inflateEnd(&clo->zstream); zlib_inflateInit(&clo->zstream);
  }
 err = zlib_inflate(&clo->zstream, Z_FINISH);
 *destLen = clo->zstream.total_out;
 if (err != Z_STREAM_END) return err;
 return Z_OK;
}

static ssize_t cloop_read_from_file(struct cloop_device *clo, struct file *f, char *buf,
  loff_t pos, size_t buf_len)
{
 size_t buf_done=0;
 while (buf_done < buf_len)
  {
   size_t size = buf_len - buf_done, size_read;
   /* kernel_read() only supports 32 bit offsets, so we use vfs_read() instead. */
   /* int size_read = kernel_read(f, pos, buf + buf_done, size); */
   mm_segment_t old_fs = get_fs();
   set_fs(get_ds());
   size_read = vfs_read(f, (void __user *)(buf + buf_done), size, &pos);
   set_fs(old_fs);

   if(size_read <= 0)
    {
     printk(KERN_ERR "%s: Read error %d at pos %Lu in file %s, "
                     "%d bytes lost.\n", cloop_name, size_read, pos,
		     file, size);
     memset(buf + buf_len - size, 0, size);
     break;
    }
   buf_done += size_read;
  }
 return buf_done;
}

/* This looks more complicated than it is */
/* Returns number of block buffer to use for this request */
static int cloop_load_buffer(struct cloop_device *clo, int blocknum)
{
 unsigned int buf_done = 0;
 unsigned long buflen;
 unsigned int buf_length;
 int ret;
 int i;
 if(blocknum > ntohl(clo->head.num_blocks) || blocknum < 0)
  {
   printk(KERN_WARNING "%s: Invalid block number %d requested.\n",
                       cloop_name, blocknum);
   return -1;
  }

 /* Quick return if the block we seek is already in one of the buffers. */
 /* Return number of buffer */
 for(i=0; i<BUFFERED_BLOCKS; i++)
  if (blocknum == clo->buffered_blocknum[i])
   {
    DEBUGP(KERN_INFO "cloop_load_buffer: Found buffered block %d\n", i);
    return i;
   }

 buf_length = be64_to_cpu(clo->offsets[blocknum+1]) - be64_to_cpu(clo->offsets[blocknum]);

/* Load one compressed block from the file. */
 cloop_read_from_file(clo, clo->backing_file, (char *)clo->compressed_buffer,
                    be64_to_cpu(clo->offsets[blocknum]), buf_length);

 buflen = ntohl(clo->head.block_size);

 /* Go to next position in the block ring buffer */
 clo->current_bufnum++;
 if(clo->current_bufnum >= BUFFERED_BLOCKS) clo->current_bufnum = 0;

 /* Do the uncompression */
 ret = uncompress(clo, clo->buffer[clo->current_bufnum], &buflen, clo->compressed_buffer,
                  buf_length);
 /* DEBUGP("cloop: buflen after uncompress: %ld\n",buflen); */
 if (ret != 0)
  {
   printk(KERN_ERR "%s: zlib decompression error %i uncompressing block %u %u/%lu/%u/%u "
          "%Lu-%Lu\n", cloop_name, ret, blocknum,
	  ntohl(clo->head.block_size), buflen, buf_length, buf_done,
	  be64_to_cpu(clo->offsets[blocknum]), be64_to_cpu(clo->offsets[blocknum+1]));
   clo->buffered_blocknum[clo->current_bufnum] = -1;
   return -1;
  }
 clo->buffered_blocknum[clo->current_bufnum] = blocknum;
 return clo->current_bufnum;
}

/* This function does all the real work. */
/* returns "uptodate" */
static int cloop_handle_request(struct cloop_device *clo, struct request *req)
{
 int buffered_blocknum = -1;
 int preloaded = 0;
 loff_t offset     = (loff_t) blk_rq_pos(req)<<9; /* req->sector<<9 */
 struct bio_vec *bvec;
 struct req_iterator iter;
 rq_for_each_segment(bvec, req, iter)
  {
   unsigned long len = bvec->bv_len;
   char *to_ptr      = kmap(bvec->bv_page) + bvec->bv_offset;
   while(len > 0)
    {
     u_int32_t length_in_buffer;
     loff_t block_offset = offset;
     u_int32_t offset_in_buffer;
     char *from_ptr;
     /* do_div (div64.h) returns the 64bit division remainder and  */
     /* puts the result in the first argument, i.e. block_offset   */
     /* becomes the blocknumber to load, and offset_in_buffer the  */
     /* position in the buffer */
     offset_in_buffer = do_div(block_offset, ntohl(clo->head.block_size));
     /* Lookup preload cache */
     if(block_offset < clo->preload_size && clo->preload_cache != NULL &&
        clo->preload_cache[block_offset] != NULL)
      { /* Copy from cache */
       preloaded = 1;
       from_ptr = clo->preload_cache[block_offset];
      }
     else
      {
       preloaded = 0;
       buffered_blocknum = cloop_load_buffer(clo,block_offset);
       if(buffered_blocknum == -1) break; /* invalid data, leave inner loop */
       /* Copy from buffer */
       from_ptr = clo->buffer[buffered_blocknum];
      }
     /* Now, at least part of what we want will be in the buffer. */
     length_in_buffer = ntohl(clo->head.block_size) - offset_in_buffer;
     if(length_in_buffer > len)
      {
/*   DEBUGP("Warning: length_in_buffer=%u > len=%u\n",
                      length_in_buffer,len); */
       length_in_buffer = len;
      }
     memcpy(to_ptr, from_ptr + offset_in_buffer, length_in_buffer);
     to_ptr      += length_in_buffer;
     len         -= length_in_buffer;
     offset      += length_in_buffer;
    } /* while inner loop */
   kunmap(bvec->bv_page);
  } /* end rq_for_each_segment*/
 return ((buffered_blocknum!=-1) || preloaded);
}

/* Adopted from loop.c, a kernel thread to handle physical reads and
 * decompression. */
static int cloop_thread(void *data)
{
 struct cloop_device *clo = data;
 current->flags |= PF_NOFREEZE;
 set_user_nice(current, -15);
 while (!kthread_should_stop()||!list_empty(&clo->clo_list))
  {
   int err;
   err = wait_event_interruptible(clo->clo_event, !list_empty(&clo->clo_list) ||
                                  kthread_should_stop());
   if(unlikely(err))
    {
     DEBUGP(KERN_ERR "cloop thread activated on error!? Continuing.\n");
     continue;
    }
   if(!list_empty(&clo->clo_list))
    {
     struct request *req;
     unsigned long flags;
     int uptodate;
     spin_lock_irq(&clo->queue_lock);
     req = list_entry(clo->clo_list.next, struct request, queuelist);
     list_del_init(&req->queuelist);
     spin_unlock_irq(&clo->queue_lock);
     uptodate = cloop_handle_request(clo, req);
     spin_lock_irqsave(&clo->queue_lock, flags);
     __blk_end_request_all(req, uptodate ? 0 : -EIO);
     spin_unlock_irqrestore(&clo->queue_lock, flags);
    }
  }
 DEBUGP(KERN_ERR "cloop_thread exited.\n");
 return 0;
}

/* This is called by the kernel block queue management every now and then,
 * with successive read requests qeued and sorted in a (hopefully)
 * "most efficient way". spin_lock_irq() is being held by the kernel. */
static void cloop_do_request(struct request_queue *q)
{
 struct request *req;
 while((req = blk_fetch_request(q)) != NULL)
  {
   struct cloop_device *clo;
   int rw;
 /* quick sanity checks */
   if (unlikely(!blk_fs_request(req))) goto error_continue;
   rw = rq_data_dir(req);
   if (unlikely(rw != READ && rw != READA))
    {
     DEBUGP("cloop_do_request: bad command\n");
     goto error_continue;
    }
   clo = req->rq_disk->private_data;
   if (unlikely(!clo->backing_file && !clo->suspended))
    {
     DEBUGP("cloop_do_request: not connected to a file\n");
     goto error_continue;
    }
   list_add_tail(&req->queuelist, &clo->clo_list); /* Add to working list for thread */
   wake_up(&clo->clo_event);    /* Wake up cloop_thread */
   continue; /* next request */
  error_continue:
   DEBUGP(KERN_ERR "cloop_do_request: Discarding request %p.\n", req);
   req->errors++;
   __blk_end_request_all(req, -EIO);
  }
}

/* Read header and offsets from already opened file */
static int cloop_set_file(int cloop_num, struct file *file, char *filename)
{
 struct cloop_device *clo=&cloop_dev[cloop_num];
 struct inode *inode;
 char *bbuf=NULL;
 unsigned int i, offsets_read, total_offsets;
 int isblkdev;
 int error = 0;
 inode = file->f_dentry->d_inode;
 isblkdev=S_ISBLK(inode->i_mode)?1:0;
 if(!isblkdev&&!S_ISREG(inode->i_mode))
  {
   printk(KERN_ERR "%s: %s not a regular file or block device\n",
		   cloop_name, filename);
   error=-EBADF; goto error_release;
  }
 clo->backing_file = file;
 clo->backing_inode= inode ;
 if(!isblkdev&&inode->i_size<sizeof(struct cloop_head))
  {
   printk(KERN_ERR "%s: %lu bytes (must be >= %u bytes)\n",
                   cloop_name, (unsigned long)inode->i_size,
		   (unsigned)sizeof(struct cloop_head));
   error=-EBADF; goto error_release;
  }
 /* In suspended mode, we have done all checks necessary - FF */
 if (clo->suspended)
   return error;
 if(isblkdev)
  {
   struct request_queue *q = bdev_get_queue(inode->i_bdev);
   blk_queue_max_sectors(clo->clo_queue, queue_max_sectors(q));
   blk_queue_max_phys_segments(clo->clo_queue, queue_max_phys_segments(q));
   blk_queue_max_hw_segments(clo->clo_queue, queue_max_hw_segments(q));
   blk_queue_max_segment_size(clo->clo_queue, queue_max_segment_size(q));
   blk_queue_segment_boundary(clo->clo_queue, queue_segment_boundary(q));
   blk_queue_merge_bvec(clo->clo_queue, q->merge_bvec_fn);
   clo->underlying_blksize = block_size(inode->i_bdev);
  }
 else
   clo->underlying_blksize = PAGE_SIZE;
 DEBUGP("Underlying blocksize is %u\n", clo->underlying_blksize);
 bbuf = cloop_malloc(clo->underlying_blksize);
 if(!bbuf)
  {
   printk(KERN_ERR "%s: out of kernel mem for block buffer (%lu bytes)\n",
                   cloop_name, (unsigned long)clo->underlying_blksize);
   error=-ENOMEM; goto error_release;
  }
 total_offsets = 1; /* Dummy total_offsets: will be filled in first time around */
 for (i = 0, offsets_read = 0; offsets_read < total_offsets; i++)
  {
   unsigned int offset = 0, num_readable;
   size_t bytes_read = cloop_read_from_file(clo, file, bbuf,
                                          i*clo->underlying_blksize,
                                          clo->underlying_blksize);
   if(bytes_read != clo->underlying_blksize)
    {
     printk(KERN_ERR "%s: Bad file, read() of first %lu bytes returned %d.\n",
                   cloop_name, (unsigned long)clo->underlying_blksize, (int)bytes_read);
     error=-EBADF;
     goto error_release;
    }
   /* Header will be in block zero */
   if(i==0)
    {
     memcpy(&clo->head, bbuf, sizeof(struct cloop_head));
     offset = sizeof(struct cloop_head);
     if (ntohl(clo->head.block_size) % 512 != 0)
      {
       printk(KERN_ERR "%s: blocksize %u not multiple of 512\n",
              cloop_name, ntohl(clo->head.block_size));
       error=-EBADF; goto error_release;
      }
     if (clo->head.preamble[0x0B]!='V'||clo->head.preamble[0x0C]<'1')
      {
       printk(KERN_ERR "%s: Cannot read old 32-bit (version 0.68) images, "
		       "please use an older version of %s for this file.\n",
		       cloop_name, cloop_name);
       error=-EBADF; goto error_release;
      }
     if (clo->head.preamble[0x0C]<'2')
      {
       printk(KERN_ERR "%s: Cannot read old architecture-dependent "
		       "(format <= 1.0) images, please use an older "
		       "version of %s for this file.\n",
		       cloop_name, cloop_name);
       error=-EBADF; goto error_release;
      }
     total_offsets=ntohl(clo->head.num_blocks)+1;
     if (!isblkdev && (sizeof(struct cloop_head)+sizeof(loff_t)*
                       total_offsets > inode->i_size))
      {
       printk(KERN_ERR "%s: file too small for %u blocks\n",
              cloop_name, ntohl(clo->head.num_blocks));
       error=-EBADF; goto error_release;
      }
     clo->offsets = cloop_malloc(sizeof(loff_t) * total_offsets);
     if (!clo->offsets)
      {
       printk(KERN_ERR "%s: out of kernel mem for offsets\n", cloop_name);
       error=-ENOMEM; goto error_release;
      }
    }
   num_readable = MIN(total_offsets - offsets_read,
                      (clo->underlying_blksize - offset)
                      / sizeof(loff_t));
   memcpy(&clo->offsets[offsets_read], bbuf+offset, num_readable * sizeof(loff_t));
   offsets_read += num_readable;
  }
  { /* Search for largest block rather than estimate. KK. */
   int i;
   for(i=0;i<total_offsets-1;i++)
    {
     loff_t d=be64_to_cpu(clo->offsets[i+1]) - be64_to_cpu(clo->offsets[i]);
     clo->largest_block=MAX(clo->largest_block,d);
    }
   printk(KERN_INFO "%s: %s: %u blocks, %u bytes/block, largest block is %lu bytes.\n",
          cloop_name, filename, ntohl(clo->head.num_blocks),
          ntohl(clo->head.block_size), clo->largest_block);
  }
/* Combo kmalloc used too large chunks (>130000). */
 {
  int i;
  for(i=0;i<BUFFERED_BLOCKS;i++)
   {
    clo->buffer[i] = cloop_malloc(ntohl(clo->head.block_size));
    if(!clo->buffer[i])
     {
      printk(KERN_ERR "%s: out of memory for buffer %lu\n",
             cloop_name, (unsigned long) ntohl(clo->head.block_size));
      error=-ENOMEM; goto error_release_free;
     }
   }
 }
 clo->compressed_buffer = cloop_malloc(clo->largest_block);
 if(!clo->compressed_buffer)
  {
   printk(KERN_ERR "%s: out of memory for compressed buffer %lu\n",
          cloop_name, clo->largest_block);
   error=-ENOMEM; goto error_release_free_buffer;
  }
 clo->zstream.workspace = cloop_malloc(zlib_inflate_workspacesize());
 if(!clo->zstream.workspace)
  {
   printk(KERN_ERR "%s: out of mem for zlib working area %u\n",
          cloop_name, zlib_inflate_workspacesize());
   error=-ENOMEM; goto error_release_free_all;
  }
 zlib_inflateInit(&clo->zstream);
 if(!isblkdev &&
    be64_to_cpu(clo->offsets[ntohl(clo->head.num_blocks)]) != inode->i_size)
  {
   printk(KERN_ERR "%s: final offset wrong (%Lu not %Lu)\n",
          cloop_name,
          be64_to_cpu(clo->offsets[ntohl(clo->head.num_blocks)]),
          inode->i_size);
   cloop_free(clo->zstream.workspace, zlib_inflate_workspacesize()); clo->zstream.workspace=NULL;
   goto error_release_free_all;
  }
 {
  int i;
  for(i=0; i<BUFFERED_BLOCKS; i++) clo->buffered_blocknum[i] = -1;
  clo->current_bufnum=0;
 }
 set_capacity(clo->clo_disk, (sector_t)(ntohl(clo->head.num_blocks)*
              (ntohl(clo->head.block_size)>>9)));
 clo->clo_thread = kthread_create(cloop_thread, clo, "cloop%d", cloop_num);
 if(IS_ERR(clo->clo_thread))
  {
   error = PTR_ERR(clo->clo_thread);
   clo->clo_thread=NULL;
   goto error_release_free_all;
  }
 if(preload > 0)
  {
   clo->preload_array_size = ((preload<=ntohl(clo->head.num_blocks))?preload:ntohl(clo->head.num_blocks));
   clo->preload_size = 0;
   if((clo->preload_cache = cloop_malloc(clo->preload_array_size * sizeof(char *))) != NULL)
    {
     int i;
     for(i=0; i<clo->preload_array_size; i++)
      {
       if((clo->preload_cache[i] = cloop_malloc(ntohl(clo->head.block_size))) == NULL)
        { /* Out of memory */
         printk(KERN_WARNING "%s: cloop_malloc(%d) failed for preload_cache[%d] (ignored).\n",
                             cloop_name, ntohl(clo->head.block_size), i);
	 break;
	}
      }
     clo->preload_size = i;
     for(i=0; i<clo->preload_size; i++)
      {
       int buffered_blocknum = cloop_load_buffer(clo,i);
       if(buffered_blocknum >= 0)
        {
	 memcpy(clo->preload_cache[i], clo->buffer[buffered_blocknum],
	        ntohl(clo->head.block_size));
	}
       else
        {
         printk(KERN_WARNING "%s: can't read block %d into preload cache, set to zero.\n",
	                     cloop_name, i);
	 memset(clo->preload_cache[i], 0, ntohl(clo->head.block_size));
	}
      }
     printk(KERN_INFO "%s: preloaded %d blocks into cache.\n", cloop_name,
                      clo->preload_size);
    }
   else
    {
     /* It is not a fatal error if cloop_malloc(clo->preload_size)
      * fails, then we just go without cache, but we should at least
      * let the user know. */
     printk(KERN_WARNING "%s: cloop_malloc(%d) failed, continuing without preloaded buffers.\n",
            cloop_name, clo->preload_size * sizeof(char *));
     clo->preload_array_size = clo->preload_size = 0;
    }
  }
 wake_up_process(clo->clo_thread);
 /* Uncheck */
 return error;
error_release_free_all:
 cloop_free(clo->compressed_buffer, clo->largest_block);
 clo->compressed_buffer=NULL;
error_release_free_buffer:
 {
  int i;
  for(i=0; i<BUFFERED_BLOCKS; i++)
   {
    if(clo->buffer[i])
     {
      cloop_free(clo->buffer[i], ntohl(clo->head.block_size));
      clo->buffer[i]=NULL;
     }
   }
 }
error_release_free:
 cloop_free(clo->offsets, sizeof(loff_t) * total_offsets);
 clo->offsets=NULL;
error_release:
 if(bbuf) cloop_free(bbuf, clo->underlying_blksize);
 clo->backing_file=NULL;
 return error;
}

/* Get file from ioctl arg (only losetup) */
static int cloop_set_fd(int cloop_num, struct file *clo_file,
                        struct block_device *bdev, unsigned int arg)
{
 struct cloop_device *clo=&cloop_dev[cloop_num];
 struct file *file=NULL;
 int error = 0;

 /* Already an allocated file present */
 if(clo->backing_file) return -EBUSY;
 file = fget(arg); /* get filp struct from ioctl arg fd */
 if(!file) return -EBADF;
 error=cloop_set_file(cloop_num,file,"losetup_file");
 if(error) fput(file);
 return error;
}

/* Drop file and free buffers, both ioctl and initial_file */
static int cloop_clr_fd(int cloop_num, struct block_device *bdev)
{
 struct cloop_device *clo = &cloop_dev[cloop_num];
 struct file *filp = clo->backing_file;
 int i;
 if(clo->refcnt > 1)	/* we needed one fd for the ioctl */
   return -EBUSY;
 if(filp==NULL) return -EINVAL;
 if(clo->clo_thread) { kthread_stop(clo->clo_thread); clo->clo_thread=NULL; }
 if(filp!=initial_file) fput(filp);
 else { filp_close(initial_file,0); initial_file=NULL; }
 clo->backing_file  = NULL;
 clo->backing_inode = NULL;
 if(clo->offsets) { cloop_free(clo->offsets, clo->underlying_blksize); clo->offsets = NULL; }
 if(clo->preload_cache)
  {
   for(i=0; i < clo->preload_size; i++)
    cloop_free(clo->preload_cache[i], ntohl(clo->head.block_size));
   cloop_free(clo->preload_cache, clo->preload_array_size * sizeof(char *));
   clo->preload_cache = NULL;
   clo->preload_size = clo->preload_array_size = 0;
  }
 for(i=0; i<BUFFERED_BLOCKS; i++)
      if(clo->buffer[i]) { cloop_free(clo->buffer[i], ntohl(clo->head.block_size)); clo->buffer[i]=NULL; }
 if(clo->compressed_buffer) { cloop_free(clo->compressed_buffer, clo->largest_block); clo->compressed_buffer = NULL; }
 zlib_inflateEnd(&clo->zstream);
 if(clo->zstream.workspace) { cloop_free(clo->zstream.workspace, zlib_inflate_workspacesize()); clo->zstream.workspace = NULL; }
 if(bdev) invalidate_bdev(bdev);
 if(clo->clo_disk) set_capacity(clo->clo_disk, 0);
 return 0;
}

static int clo_suspend_fd(int cloop_num)
{
 struct cloop_device *clo = &cloop_dev[cloop_num];
 struct file *filp = clo->backing_file;
 if(filp==NULL || clo->suspended) return -EINVAL;
 /* Suspend all running requests - FF */
 clo->suspended=1;
 if(filp!=initial_file) fput(filp);
 else { filp_close(initial_file,0); initial_file=NULL; }
 clo->backing_file  = NULL;
 clo->backing_inode = NULL;
 return 0;
}

/* Copied from loop.c, stripped down to the really necessary */
static int cloop_set_status(struct cloop_device *clo,
                            const struct loop_info64 *info)
{
 if (!clo->backing_file) return -ENXIO;
 memcpy(clo->clo_file_name, info->lo_file_name, LO_NAME_SIZE);
 clo->clo_file_name[LO_NAME_SIZE-1] = 0;
 return 0;
}

static int cloop_get_status(struct cloop_device *clo,
                            struct loop_info64 *info)
{
 struct file *file = clo->backing_file;
 struct kstat stat;
 int err;
 if (!file) return -ENXIO;
 err = vfs_getattr(file->f_path.mnt, file->f_path.dentry, &stat);
 if (err) return err;
 memset(info, 0, sizeof(*info));
 info->lo_number  = clo->clo_number;
 info->lo_device  = huge_encode_dev(stat.dev);
 info->lo_inode   = stat.ino;
 info->lo_rdevice = huge_encode_dev(clo->isblkdev ? stat.rdev : stat.dev);
 info->lo_offset  = 0;
 info->lo_sizelimit = 0;
 info->lo_flags   = 0;
 memcpy(info->lo_file_name, clo->clo_file_name, LO_NAME_SIZE);
 return 0;
}

static void cloop_info64_from_old(const struct loop_info *info,
                                  struct loop_info64 *info64)
{
 memset(info64, 0, sizeof(*info64));
 info64->lo_number = info->lo_number;
 info64->lo_device = info->lo_device;
 info64->lo_inode = info->lo_inode;
 info64->lo_rdevice = info->lo_rdevice;
 info64->lo_offset = info->lo_offset;
 info64->lo_sizelimit = 0;
 info64->lo_flags = info->lo_flags;
 info64->lo_init[0] = info->lo_init[0];
 info64->lo_init[1] = info->lo_init[1];
 memcpy(info64->lo_file_name, info->lo_name, LO_NAME_SIZE);
}

static int cloop_info64_to_old(const struct loop_info64 *info64,
                               struct loop_info *info)
{
 memset(info, 0, sizeof(*info));
 info->lo_number = info64->lo_number;
 info->lo_device = info64->lo_device;
 info->lo_inode = info64->lo_inode;
 info->lo_rdevice = info64->lo_rdevice;
 info->lo_offset = info64->lo_offset;
 info->lo_flags = info64->lo_flags;
 info->lo_init[0] = info64->lo_init[0];
 info->lo_init[1] = info64->lo_init[1];
 memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
 return 0;
}

static int cloop_set_status_old(struct cloop_device *clo,
                                const struct loop_info __user *arg)
{
 struct loop_info info;
 struct loop_info64 info64;

 if (copy_from_user(&info, arg, sizeof (struct loop_info))) return -EFAULT;
 cloop_info64_from_old(&info, &info64);
 return cloop_set_status(clo, &info64);
}

static int cloop_set_status64(struct cloop_device *clo,
                              const struct loop_info64 __user *arg)
{
 struct loop_info64 info64;
 if (copy_from_user(&info64, arg, sizeof (struct loop_info64)))
  return -EFAULT;
 return cloop_set_status(clo, &info64);
}

static int cloop_get_status_old(struct cloop_device *clo,
                                struct loop_info __user *arg)
{
 struct loop_info info;
 struct loop_info64 info64;
 int err = 0;

 if (!arg) err = -EINVAL;
 if (!err) err = cloop_get_status(clo, &info64);
 if (!err) err = cloop_info64_to_old(&info64, &info);
 if (!err && copy_to_user(arg, &info, sizeof(info))) err = -EFAULT;
 return err;
}

static int cloop_get_status64(struct cloop_device *clo,
                              struct loop_info64 __user *arg)
{
 struct loop_info64 info64;
 int err = 0;
 if (!arg) err = -EINVAL;
 if (!err) err = cloop_get_status(clo, &info64);
 if (!err && copy_to_user(arg, &info64, sizeof(info64))) err = -EFAULT;
 return err;
}
/* EOF get/set_status */


static int cloop_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
 struct cloop_device *clo;
 int cloop_num, err=0;
 if (!bdev) return -EINVAL;
 cloop_num = MINOR(bdev->bd_dev);
 if (cloop_num < 0 || cloop_num > cloop_count-1) return -ENODEV;
 clo = &cloop_dev[cloop_num];
 switch (cmd)
  { /* We use the same ioctls that loop does */
   case LOOP_CHANGE_FD:
   case LOOP_SET_FD:
    err = cloop_set_fd(cloop_num, NULL, bdev, arg);
    if (err == 0 && clo->suspended)
     {
      /* Okay, we have again a backing file - get reqs again - FF */
      clo->suspended=0;
     }
     break;
   case LOOP_CLR_FD:
     err = cloop_clr_fd(cloop_num, bdev);
     break;
   case LOOP_SET_STATUS:
    err = cloop_set_status_old(clo, (struct loop_info __user *) arg);
    break;
   case LOOP_GET_STATUS:
    err = cloop_get_status_old(clo, (struct loop_info __user *) arg);
    break;
   case LOOP_SET_STATUS64:
    err = cloop_set_status64(clo, (struct loop_info64 __user *) arg);
    break;
   case LOOP_GET_STATUS64:
    err = cloop_get_status64(clo, (struct loop_info64 __user *) arg);
    break;
   case CLOOP_SUSPEND:
     err = clo_suspend_fd(cloop_num);
     break;
   default:
     err = -EINVAL;
  }
 return err;
}

static int cloop_open(struct block_device *bdev, fmode_t mode)
{
 int cloop_num;
 if(!bdev) return -EINVAL;
 cloop_num=MINOR(bdev->bd_dev);
 if(cloop_num > cloop_count-1) return -ENODEV;
 /* Allow write open for ioctl, but not for mount. */
 /* losetup uses write-open and flags=0x8002 to set a new file */
 if(mode & FMODE_WRITE)
  {
   printk(KERN_WARNING "%s: Can't open device read-write in mode 0x%x\n", cloop_name, mode);
   return -EROFS;
  }
 cloop_dev[cloop_num].refcnt+=1;
 return 0;
}

static int cloop_close(struct gendisk *disk, fmode_t mode)
{
 int cloop_num, err=0;
 if(!disk) return 0;
 cloop_num=((struct cloop_device *)disk->private_data)->clo_number;
 if(cloop_num < 0 || cloop_num > (cloop_count-1)) return 0;
 cloop_dev[cloop_num].refcnt-=1;
 return err;
}

static struct block_device_operations clo_fops =
{
        owner:		THIS_MODULE,
        open:           cloop_open,
        release:        cloop_close,
	locked_ioctl:   cloop_ioctl
};

static int cloop_register_blkdev(int major_nr)
{
 return register_blkdev(major_nr, cloop_name);
}

static int cloop_unregister_blkdev(void)
{
 unregister_blkdev(cloop_major, cloop_name);
 return 0;
}

static int cloop_alloc(int cloop_num)
{
 struct cloop_device *clo = &cloop_dev[cloop_num];
 memset(clo, 0, sizeof(struct cloop_device));
 clo->clo_number = cloop_num;
 clo->clo_thread = NULL;
 init_waitqueue_head(&clo->clo_event);
 spin_lock_init(&clo->queue_lock);
 INIT_LIST_HEAD(&clo->clo_list);
 clo->clo_queue = blk_init_queue(cloop_do_request, &clo->queue_lock);
 if(!clo->clo_queue)
  {
   printk(KERN_ERR "%s: Unable to alloc queue[%d]\n", cloop_name, cloop_num);
   goto error_out;
  }
 clo->clo_queue->queuedata = clo;
 clo->clo_disk = alloc_disk(1);
 if(!clo->clo_disk)
  {
   printk(KERN_ERR "%s: Unable to alloc disk[%d]\n", cloop_name, cloop_num);
   goto error_disk;
  }
 clo->clo_disk->major = cloop_major;
 clo->clo_disk->first_minor = cloop_num;
 clo->clo_disk->fops = &clo_fops;
 clo->clo_disk->queue = clo->clo_queue;
 clo->clo_disk->private_data = clo;
 sprintf(clo->clo_disk->disk_name, "%s%d", cloop_name, cloop_num);
 add_disk(clo->clo_disk);
 return 0;
error_disk:
 blk_cleanup_queue(clo->clo_queue);
error_out:
 return -ENOMEM;
}

static void cloop_dealloc(int cloop_num)
{
 struct cloop_device *clo = &cloop_dev[cloop_num];
 del_gendisk(clo->clo_disk);
 blk_cleanup_queue(clo->clo_queue);
 put_disk(clo->clo_disk);
}

static int __init cloop_init(void)
{
 int error=0;
 printk("%s: Initializing %s v"CLOOP_VERSION"\n", cloop_name, cloop_name);
 cloop_count=0;
 cloop_major=MAJOR_NR;
 if(cloop_register_blkdev(MAJOR_NR))
  {
   printk(KERN_WARNING "%s: Unable to get major device %d\n", cloop_name,
          MAJOR_NR);
   /* Try dynamic allocation */
   if((cloop_major=cloop_register_blkdev(0))<0)
    {
     printk(KERN_ERR "%s: Unable to get dynamic major device\n", cloop_name);
     return -EIO;
    }
   printk(KERN_INFO "%s: Got dynamic major device %d, "
                    "mknod /dev/%s b %d 0\n",
          cloop_name, cloop_major, cloop_name, cloop_major);
  }
 while(cloop_count<cloop_max)
  if((error=cloop_alloc(cloop_count))!=0) break;
  else ++cloop_count;
 if(!cloop_count) goto init_out_dealloc;
 printk(KERN_INFO "%s: loaded (max %d devices)\n", cloop_name, cloop_count);
 if(file) /* global file name for first cloop-Device is a module option string. */
  {
   int namelen = strlen(file);
   if(namelen<1 ||
      (initial_file=filp_open(file,O_RDONLY|O_LARGEFILE,0x00))==NULL ||
      IS_ERR(initial_file))
    {
     error=PTR_ERR(initial_file);
     if(!error) error=-EINVAL;
     initial_file=NULL; /* if IS_ERR, it's NOT open. */
    }
   else
     error=cloop_set_file(0,initial_file,file);
   if(error)
    {
     printk(KERN_ERR
            "%s: Unable to get file %s for cloop device, error %d\n",
            cloop_name, file, error);
     goto init_out_dealloc;
    }
   if(namelen >= LO_NAME_SIZE) namelen = LO_NAME_SIZE-1;
   memcpy(cloop_dev[0].clo_file_name, file, namelen);
   cloop_dev[0].clo_file_name[namelen] = 0;
  }
 return 0;
init_out_dealloc:
 while (cloop_count>0) cloop_dealloc(--cloop_count);
 cloop_unregister_blkdev();
 return error;
}

static void __exit cloop_exit(void)
{
 int error=0;
 if((error=cloop_unregister_blkdev())!=0)
  {
   printk(KERN_ERR "%s: cannot unregister block device\n", cloop_name);
   return;
  }
 while(cloop_count>0)
  {
   --cloop_count;
   if(cloop_dev[cloop_count].backing_file) cloop_clr_fd(cloop_count, NULL);
   cloop_dealloc(cloop_count);
  }
 printk("%s: unloaded.\n", cloop_name);
}

/* The cloop init and exit function registration (especially needed for Kernel 2.6) */
module_init(cloop_init);
module_exit(cloop_exit);

#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);
