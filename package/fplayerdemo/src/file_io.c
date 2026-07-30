// SPDX-License-Identifier: GPL-2.0
#include "fplayerdemo.h"

int map_file_ro(const char *path, uint8_t **data, size_t *size)
{
	struct stat st;
	void *map;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		return -1;
	}

	if (fstat(fd, &st) || st.st_size <= 0) {
		perror("fstat");
		close(fd);
		return -1;
	}

	if ((uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "%s too large to map on this target: %ju bytes\n",
			path, (uintmax_t)st.st_size);
		close(fd);
		return -1;
	}

	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap %s size=%ju failed: %s\n",
			path, (uintmax_t)st.st_size, strerror(errno));
		return -1;
	}

	*data = map;
	*size = (size_t)st.st_size;
	return 0;
}

static uint64_t discard_file_window(const uint8_t *file, size_t file_size,
				    uint64_t start, uint64_t end)
{
	static bool warned;
	long page = sysconf(_SC_PAGESIZE);
	size_t len;

	if (page <= 0)
		page = 4096;
	if (start >= end || start >= file_size)
		return start;
	if (end > file_size)
		end = file_size;

	start = (start + (uint64_t)page - 1) / (uint64_t)page *
		(uint64_t)page;
	end = end / (uint64_t)page * (uint64_t)page;
	if (start >= end)
		return start;

	len = (size_t)(end - start);
	if (madvise((void *)(file + start), len, MADV_DONTNEED) && !warned) {
		fprintf(stderr, "discard madvise failed: %s\n",
			strerror(errno));
		warned = true;
	}

	return end;
}

/*
 * Drop mmap pages that are far behind the playback cursor. This bounds page
 * cache pressure on CMA-constrained targets without asking the kernel to read
 * future pages.
 */

void file_window_init(struct file_window *win, const uint8_t *file,
			     size_t file_size, bool enabled, bool allow_discard)
{
	memset(win, 0, sizeof(*win));
	win->file = file;
	win->file_size = file_size;
	win->enabled = enabled;
	win->allow_discard = allow_discard;
}

/*
 * Rewind the discard bookkeeping when a seamless loop starts from offset 0.
 */
void file_window_reset(struct file_window *win)
{
	win->discard_end = 0;
}

void file_window_advance(struct file_window *win, uint64_t offset)
{
	if (!win->enabled)
		return;
	if (offset >= win->file_size)
		return;

	if (!win->allow_discard || offset <= FILE_KEEP_BEHIND_BYTES)
		return;

	/*
	 * Drop pages that are more than FILE_KEEP_BEHIND_BYTES behind the read
	 * cursor, advancing in FILE_DISCARD_STEP_BYTES chunks to avoid a
	 * madvise syscall on every sample.
	 */
	{
		uint64_t keep_from = offset - FILE_KEEP_BEHIND_BYTES;

		if (keep_from >= win->discard_end + FILE_DISCARD_STEP_BYTES)
			win->discard_end = discard_file_window(win->file,
							       win->file_size,
							       win->discard_end,
							       keep_from);
	}
}
