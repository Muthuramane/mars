// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING
//#define IO_DEBUGGING

#define MAX_RESERVE PAGE_SIZE

#include "lib_log.h"

atomic_t global_mref_flying = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(global_mref_flying);

void exit_logst(struct log_status *logst)
{
	int count = 0;
	log_flush(logst);
	while (atomic_read(&logst->mref_flying) > 0) {
		if (!count++)
			MARS_DBG("waiting for IO terminating...");
		brick_msleep(500);
	}
	if (logst->read_mref) {
		MARS_DBG("putting read_mref\n");
		GENERIC_INPUT_CALL(logst->input, mref_put, logst->read_mref);
		logst->read_mref = NULL;
	}
	if (logst->log_mref) {
		MARS_DBG("putting log_mref\n");
		GENERIC_INPUT_CALL(logst->input, mref_put, logst->log_mref);
		logst->log_mref = NULL;
	}
}
EXPORT_SYMBOL_GPL(exit_logst);

void init_logst(struct log_status *logst, struct mars_input *input, loff_t start_pos)
{
	exit_logst(logst);

	memset(logst, 0, sizeof(struct log_status));

	logst->input = input;
	logst->brick = input->brick;
	logst->log_pos = start_pos;
	init_waitqueue_head(&logst->event);
}
EXPORT_SYMBOL_GPL(init_logst);

#define MARS_LOG_CB_MAX 32

struct log_cb_info {
	struct mref_object *mref;
	struct log_status *logst;
	struct semaphore mutex;
	atomic_t refcount;
	int nr_cb;
	void (*preios[MARS_LOG_CB_MAX])(void *private);
	void (*endios[MARS_LOG_CB_MAX])(void *private, int error);
	void *privates[MARS_LOG_CB_MAX];
};

static
void put_log_cb_info(struct log_cb_info *cb_info)
{
	if (atomic_dec_and_test(&cb_info->refcount)) {
		brick_mem_free(cb_info);
	}
}

static
void _do_callbacks(struct log_cb_info *cb_info, int error, bool both)
{
	int i;
	down(&cb_info->mutex);
	for (i = 0; i < cb_info->nr_cb; i++) {
		void (*pre_fn)(void *private);
		void (*end_fn)(void *private, int error);
		pre_fn = cb_info->preios[i];
		cb_info->preios[i] = NULL;
		if (pre_fn) {
			pre_fn(cb_info->privates[i]);
		}
		if (!both)
			continue;
		end_fn = cb_info->endios[i];
		cb_info->endios[i] = NULL;
		if (end_fn) {
			end_fn(cb_info->privates[i], error);
		}
	}
	up(&cb_info->mutex);
}

static
void log_write_endio(struct generic_callback *cb)
{
	struct log_cb_info *cb_info = cb->cb_private;
	struct log_status *logst;

	CHECK_PTR(cb_info, err);

	if (cb_info->mref) {
		mars_trace(cb_info->mref, "log_endio");
		mars_log_trace(cb_info->mref);
	}

	logst = cb_info->logst;
	CHECK_PTR(logst, done);

	MARS_IO("nr_cb = %d\n", cb_info->nr_cb);

	_do_callbacks(cb_info, cb->cb_error, true);

 done:
	put_log_cb_info(cb_info);
	atomic_dec(&logst->mref_flying);
	atomic_dec(&global_mref_flying);
	if (logst->signal_event)
		wake_up_interruptible(logst->signal_event);

	return;

err:
	MARS_FAT("internal pointer corruption\n");
}

void log_flush(struct log_status *logst)
{
	struct mref_object *mref = logst->log_mref;
	struct log_cb_info *cb_info;
	int gap;

	if (!mref || !logst->count)
		return;

	gap = 0;
	if (logst->align_size > 0) {
		// round up to next alignment border
		int align_offset = logst->offset & (logst->align_size-1);
		if (align_offset > 0) {
			int restlen = mref->ref_len - logst->offset;
			gap = logst->align_size - align_offset;
			if (gap > restlen) {
				gap = restlen;
			}
		}
	}
	if (gap > 0) {
		// don't leak information from kernelspace
		memset(mref->ref_data + logst->offset, 0, gap);
		logst->offset += gap;
	}
	mref->ref_len = logst->offset;
	logst->log_pos += logst->offset;
	memcpy(&logst->log_pos_stamp, &logst->tmp_pos_stamp, sizeof(logst->log_pos_stamp));

	cb_info = logst->private;
	logst->private = NULL;
	SETUP_CALLBACK(mref, log_write_endio, cb_info);
	cb_info->logst = logst;
	mref->ref_rw = 1;

	mars_trace(mref, "log_flush");

	atomic_inc(&logst->mref_flying);
	atomic_inc(&global_mref_flying);

	_do_callbacks(cb_info, 0, false);

	GENERIC_INPUT_CALL(logst->input, mref_io, mref);
	GENERIC_INPUT_CALL(logst->input, mref_put, mref);

	logst->offset = 0;
	logst->count = 0;
	logst->log_mref = NULL;

	put_log_cb_info(cb_info);
}
EXPORT_SYMBOL_GPL(log_flush);

void *log_reserve(struct log_status *logst, struct log_header *lh)
{
	struct log_cb_info *cb_info = logst->private;
	struct mref_object *mref;
	void *data;
	short total_len = lh->l_len + OVERHEAD;
	int offset;
	int status;

	if (unlikely(lh->l_len <= 0 || lh->l_len > MAX_RESERVE)) {
		MARS_ERR("trying to write %d bytes, max allowed = %d\n", lh->l_len, (int)PAGE_SIZE);
		goto err;
	}

	MARS_IO("reserving %d bytes at %lld\n", lh->l_len, logst->log_pos);

	mref = logst->log_mref;
	if ((mref && total_len > mref->ref_len - logst->offset)
	   || !cb_info || cb_info->nr_cb >= MARS_LOG_CB_MAX) {
		log_flush(logst);
	}

	mref = logst->log_mref;
	if (!mref) {
		if (unlikely(logst->private)) {
			MARS_ERR("oops\n");
			brick_mem_free(logst->private);
		}
		logst->private = brick_zmem_alloc(sizeof(struct log_cb_info));
		if (unlikely(!logst->private)) {
			MARS_ERR("no memory\n");
			goto err;
		}
		cb_info = logst->private;
		sema_init(&cb_info->mutex, 1);
		atomic_set(&cb_info->refcount, 2);

		mref = mars_alloc_mref(logst->brick);
		if (unlikely(!mref)) {
			MARS_ERR("no mref\n");
			goto err;
		}
		cb_info->mref = mref;

		mref->ref_pos = logst->log_pos;
		mref->ref_len = total_len;
		mref->ref_may_write = WRITE;
		mref->ref_prio = logst->io_prio;
		if (logst->chunk_size > 0) {
			int chunk_offset;
			int chunk_rest;
			chunk_offset = logst->log_pos & (loff_t)(logst->chunk_size - 1);
			chunk_rest = logst->chunk_size - chunk_offset;
			while (chunk_rest < total_len) {
				chunk_rest += logst->chunk_size;
			}
			mref->ref_len = chunk_rest;
		}

		for (;;) {
			status = GENERIC_INPUT_CALL(logst->input, mref_get, mref);
			if (likely(status >= 0)) {
				break;
			}
			if (status != -ENOMEM && status != -EAGAIN) {
				MARS_ERR("mref_get() failed, status = %d\n", status);
				goto err_free;
			}
			brick_msleep(100);
		}

		mars_trace(mref, "log_start");

		if (unlikely(mref->ref_len < total_len)) {
			MARS_ERR("ref_len = %d total_len = %d\n", mref->ref_len, total_len);
			goto put;
		}

		logst->offset = 0;
		logst->log_mref = mref;
	}

	offset = logst->offset;
	data = mref->ref_data;
	DATA_PUT(data, offset, START_MAGIC);
	DATA_PUT(data, offset, (char)FORMAT_VERSION);
	logst->validflag_offset = offset;
	DATA_PUT(data, offset, (char)0); // valid_flag
	DATA_PUT(data, offset, total_len); // start of next header
	DATA_PUT(data, offset, lh->l_stamp.tv_sec);
	DATA_PUT(data, offset, lh->l_stamp.tv_nsec);
	DATA_PUT(data, offset, lh->l_pos);
	logst->reallen_offset = offset;
	DATA_PUT(data, offset, lh->l_len);
	DATA_PUT(data, offset, lh->l_extra_len);
	DATA_PUT(data, offset, lh->l_code);
	DATA_PUT(data, offset, lh->l_extra);

	// remember the last timestamp
	memcpy(&logst->tmp_pos_stamp, &lh->l_stamp, sizeof(logst->tmp_pos_stamp));

	logst->payload_offset = offset;
	logst->payload_len = lh->l_len;

	return data + offset;

put:
	GENERIC_INPUT_CALL(logst->input, mref_put, mref);
	logst->log_mref = NULL;
	return NULL;

err_free:
	mars_free_mref(mref);
	if (logst->private) {
		// TODO: if callbacks are already registered, call them here with some error code
		brick_mem_free(logst->private);
		logst->private = NULL;
	}
err:
	return NULL;
}
EXPORT_SYMBOL_GPL(log_reserve);

bool log_finalize(struct log_status *logst, int len, void (*preio)(void *private), void (*endio)(void *private, int error), void *private)
{
	struct mref_object *mref = logst->log_mref;
	struct log_cb_info *cb_info = logst->private;
	struct timespec now;
	void *data;
	int offset;
	int restlen;
	int nr_cb;
	int crc;
	bool ok = false;

	CHECK_PTR(mref, err);

	if (unlikely(len > logst->payload_len)) {
		MARS_ERR("trying to write more than reserved (%d > %d)\n", len, logst->payload_len);
		goto err;
	}
	restlen = mref->ref_len - logst->offset;
	if (unlikely(len + END_OVERHEAD > restlen)) {
		MARS_ERR("trying to write more than available (%d > %d)\n", len, (int)(restlen - END_OVERHEAD));
		goto err;
	}
	if (unlikely(!cb_info || cb_info->nr_cb >= MARS_LOG_CB_MAX)) {
		MARS_ERR("too many endio() calls\n");
		goto err;
	}

	data = mref->ref_data;

	crc = 0;
	if (logst->do_crc) {
		unsigned char checksum[mars_digest_size];
		mars_digest(checksum, data + logst->payload_offset, len);
		crc = *(int*)checksum;
	}

	/* Correct the length in the header.
	 */
	offset = logst->reallen_offset;
	DATA_PUT(data, offset, len);

	/* Write the trailer.
	 */
	offset = logst->payload_offset + len;
	DATA_PUT(data, offset, END_MAGIC);
	DATA_PUT(data, offset, crc);
	DATA_PUT(data, offset, (char)1);  // valid_flag copy
	DATA_PUT(data, offset, (char)0);  // spare
	DATA_PUT(data, offset, (short)0); // spare
	DATA_PUT(data, offset, (int)0);   // spare
	get_lamport(&now);    // when the log entry was ready.
	DATA_PUT(data, offset, now.tv_sec);  
	DATA_PUT(data, offset, now.tv_nsec);

	if (unlikely(offset > mref->ref_len)) {
		MARS_ERR("length calculation was wrong: %d > %d\n", offset, mref->ref_len);
		goto err;
	}
	logst->offset = offset;

	/* This must come last. In case of incomplete
	 * or even overlapping disk transfers, this indicates
	 * the completeness / integrity of the payload at
	 * the time of starting the transfer.
	 */
	offset = logst->validflag_offset;
	DATA_PUT(data, offset, (char)1);

	nr_cb = cb_info->nr_cb++;
	cb_info->preios[nr_cb] = preio;
	cb_info->endios[nr_cb] = endio;
	cb_info->privates[nr_cb] = private;

	logst->count++;
	ok = true;

err:
	return ok;
}
EXPORT_SYMBOL_GPL(log_finalize);

#define SCAN_TXT "at file_pos = %lld file_offset = %d scan_offset = %d (%lld) test_offset = %d (%lld) restlen = %d: "
#define SCAN_PAR file_pos, file_offset, offset, file_pos + file_offset + offset, i, file_pos + file_offset + i, restlen

static
int log_scan(void *buf, int len, loff_t file_pos, int file_offset, struct log_header *lh, void **payload, int *payload_len)
{
	bool dirty = false;
	int offset;
	int i;

	*payload = NULL;
	*payload_len = 0;

	for (i = 0; i < len; i += sizeof(long)) {
		long long start_magic;
		char format_version;
		char valid_flag;
		short total_len;
		long long end_magic;
		char valid_copy;

		int restlen;
		int found_offset;

		offset = i;
		DATA_GET(buf, offset, start_magic);
		if (start_magic != START_MAGIC) {
			if (start_magic != 0)
				dirty = true;
			continue;
		}

		restlen = len - i;
		if (restlen < START_OVERHEAD) {
			MARS_ERR(SCAN_TXT "magic found, but restlen is too small\n", SCAN_PAR);
			return -EBADMSG;
		}

		DATA_GET(buf, offset, format_version);
		if (format_version != FORMAT_VERSION) {
			MARS_ERR(SCAN_TXT "found unknown data format %d\n", SCAN_PAR, (int)format_version);
			return -EBADMSG;
		}
		DATA_GET(buf, offset, valid_flag);
		if (!valid_flag) {
			MARS_WRN(SCAN_TXT "data is marked invalid (was there a short write?)\n", SCAN_PAR);
			continue;
		}
		DATA_GET(buf, offset, total_len);
		if (total_len > restlen) {
			MARS_ERR(SCAN_TXT "data is longer than expected, total_len = %d restlen = %d\n", SCAN_PAR, total_len, restlen);
			return -EBADMSG;
		}

		memset(lh, 0, sizeof(struct log_header));

		DATA_GET(buf, offset, lh->l_stamp.tv_sec);
		DATA_GET(buf, offset, lh->l_stamp.tv_nsec);
		DATA_GET(buf, offset, lh->l_pos);
		DATA_GET(buf, offset, lh->l_len);
		DATA_GET(buf, offset, lh->l_extra_len);
		DATA_GET(buf, offset, lh->l_code);
		DATA_GET(buf, offset, lh->l_extra);

		found_offset = offset;
		offset += lh->l_len;

		restlen = len - offset;
		if (restlen < END_OVERHEAD) {
			MARS_WRN(SCAN_TXT "magic found, but restlen %d is too small\n", SCAN_PAR, restlen);
			continue;
		}

		DATA_GET(buf, offset, end_magic);
		if (end_magic != END_MAGIC) {
			MARS_WRN(SCAN_TXT "bad end_magic 0x%llx\n", SCAN_PAR, end_magic);
			continue;
		}
		DATA_GET(buf, offset, lh->l_crc);
		DATA_GET(buf, offset, valid_copy);
		if (valid_copy != 1) {
			MARS_WRN(SCAN_TXT "found uncompleted / invalid data, len = %d, valid_flag = %d\n", SCAN_PAR, lh->l_len, (int)valid_copy);
			continue;
		}
		// skip spares
		offset += 3 + 4;
		DATA_GET(buf, offset, lh->l_written.tv_sec);
		DATA_GET(buf, offset, lh->l_written.tv_nsec);

		if (lh->l_crc) {
			unsigned char checksum[mars_digest_size];
			mars_digest(checksum, buf + found_offset, lh->l_len);
			if (unlikely(*(int*)checksum != lh->l_crc)) {
				MARS_ERR("data checksumming mismatch, length = %d\n", lh->l_len);
				return -EBADMSG;
			}
		}

		// last check
		if (total_len != offset - i) {
			MARS_WRN(SCAN_TXT "size mismatch: %d != %d\n", SCAN_PAR, total_len, offset - i);
			// just warn, but no consequences: better use the data, it has been checked by lots of magics
		}

		// Success...
		*payload = buf + found_offset;
		*payload_len = lh->l_len;
		goto done;
	}
	offset = i;

done:
	// don't cry when nullbytes have been skipped
	if (i > 0 && dirty) {
		MARS_WRN("skipped %d dirty bytes at offset %d to find valid data\n", i, offset);
	}
	return offset;
}

static
void log_read_endio(struct generic_callback *cb)
{
	struct log_status *logst = cb->cb_private;

	CHECK_PTR(logst, err);
	logst->error_code = cb->cb_error;
	logst->got = true;
	wake_up_interruptible(&logst->event);
	return;

err:
	MARS_FAT("internal pointer corruption\n");
}


int log_read(struct log_status *logst, struct log_header *lh, void **payload, int *payload_len)
{
	struct mref_object *mref;
	int old_offset;
	int status;

restart:
	status = 0;
	mref = logst->read_mref;
	if (!mref || logst->do_free) {
		int chunk_offset;
		int chunk_rest;

		if (mref) {
			logst->log_pos += logst->offset;
			GENERIC_INPUT_CALL(logst->input, mref_put, mref);
			logst->read_mref = NULL;
			logst->offset = 0;
		}

		mref = mars_alloc_mref(logst->brick);
		if (unlikely(!mref)) {
			MARS_ERR("no mref\n");
			goto done;
		}
		mref->ref_pos = logst->log_pos;
		chunk_offset = logst->log_pos & (loff_t)(logst->chunk_size - 1);
		chunk_rest = logst->chunk_size - chunk_offset;
		mref->ref_len = chunk_rest + logst->chunk_size * 4;
		MARS_IO("log_pos = %lld chunk_offset = %d chunk_rest = %d ref_len = %d\n", logst->log_pos, chunk_offset, chunk_rest, mref->ref_len);
		mref->ref_prio = logst->io_prio;

		status = GENERIC_INPUT_CALL(logst->input, mref_get, mref);
		if (unlikely(status < 0)) {
			if (status != -ENODATA) {
				MARS_ERR("mref_get() failed, status = %d\n", status);
			}
			goto done_free;
		}

		SETUP_CALLBACK(mref, log_read_endio, logst);
		mref->ref_rw = READ;
		logst->offset = 0;
		logst->got = false;
		logst->do_free = false;

		GENERIC_INPUT_CALL(logst->input, mref_io, mref);

		wait_event_interruptible_timeout(logst->event, logst->got, 60 * HZ);
		status = -EIO;
		if (!logst->got)
			goto done_put;
		status = logst->error_code;
		if (status < 0)
			goto done_put;
		logst->read_mref = mref;
	}

	status = log_scan(mref->ref_data + logst->offset,
			  mref->ref_len - logst->offset,
			  mref->ref_pos,
			  logst->offset,
			  lh,
			  payload,
			  payload_len);

	if (unlikely(status == 0)) {
		MARS_ERR("bad logfile scan\n");
		status = -EINVAL;
	}
	if (unlikely(status < 0)) {
		goto done_put;
	}

	// memorize success
	logst->offset += status;
	if (logst->offset > mref->ref_len - logst->chunk_size) {
		logst->do_free = true;
	}

done:
	if (status == -ENODATA) {
		status = 0; // indicates EOF
	}
	return status;

done_put:
	old_offset = logst->offset;
	if (mref) {
		logst->log_pos += logst->offset;
		GENERIC_INPUT_CALL(logst->input, mref_put, mref);
		logst->read_mref = NULL;
		logst->offset = 0;
	}
	if (status == -EAGAIN && old_offset > 0) {
		goto restart;
	}
	goto done;

done_free:
	if (mref) {
		mars_free_mref(mref);
	}
	logst->read_mref = NULL;
	goto done;

}
EXPORT_SYMBOL_GPL(log_read);


////////////////// module init stuff /////////////////////////

int __init init_log_format(void)
{
	MARS_INF("init_log_format()\n");
	return 0;
}

void __exit exit_log_format(void)
{
	MARS_INF("exit_log_format()\n");
}

#ifndef CONFIG_MARS_HAVE_BIGMODULE
MODULE_DESCRIPTION("MARS log_format infrastucture");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_log_format);
module_exit(exit_log_format);
#endif
