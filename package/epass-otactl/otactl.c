#define _GNU_SOURCE

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include <libmtd.h>

#define BOOTCTRL_MAGIC 0x43425045U
#define BOOTCTRL_VERSION_LEGACY 2U
#define BOOTCTRL_VERSION 3U
#define BOOTCTRL_MTD_NAME "bootctrl"
#define BOOTCTRL_SLOTS 2
#define BOOT_MODE_NORMAL 0U
#define BOOT_MODE_UPDATE 1U
#define KERNEL_MAX_SIZE (8U * 1024U * 1024U)
#define ROOTFS_MAX_SIZE (20U * 1024U * 1024U)
#define SHA256_SIZE 32U

struct bootctrl_record {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t sequence;
	uint8_t mode;
	uint8_t reserved0[3];
	uint32_t kernel_size;
	uint32_t rootfs_size;
	uint8_t kernel_sha256[SHA256_SIZE];
	uint8_t rootfs_sha256[SHA256_SIZE];
	uint8_t reserved[4];
	uint32_t crc;
} __attribute__((packed));

struct boot_state {
	uint32_t sequence;
	uint8_t mode;
	uint32_t kernel_size;
	uint32_t rootfs_size;
	uint8_t kernel_sha256[SHA256_SIZE];
	uint8_t rootfs_sha256[SHA256_SIZE];
	int source;
};

struct bootctrl_mtd {
	libmtd_t lib;
	struct mtd_dev_info info;
	uint8_t *page;
	int fd;
};

_Static_assert(sizeof(struct bootctrl_record) == 96,
	       "bootctrl record must remain 96 bytes");

static uint32_t crc32_bytes(const void *data, size_t length)
{
	const uint8_t *bytes = data;
	uint32_t crc = UINT32_MAX;
	size_t i;
	int bit;

	for (i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			      (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
	}

	return ~crc;
}

static int hash_is_zero(const uint8_t hash[SHA256_SIZE])
{
	size_t i;

	for (i = 0; i < SHA256_SIZE; i++) {
		if (hash[i])
			return 0;
	}

	return 1;
}

static int update_metadata_valid(uint32_t size, uint32_t maximum,
				 const uint8_t hash[SHA256_SIZE])
{
	if (!size)
		return hash_is_zero(hash);

	return size <= maximum && !hash_is_zero(hash);
}

static int record_valid(const struct bootctrl_record *record)
{
	uint32_t kernel_size = le32toh(record->kernel_size);
	uint32_t rootfs_size = le32toh(record->rootfs_size);
	uint16_t version = le16toh(record->version);
	uint32_t expected;

	if (le32toh(record->magic) != BOOTCTRL_MAGIC ||
	    (version != BOOTCTRL_VERSION_LEGACY &&
	     version != BOOTCTRL_VERSION) ||
	    le16toh(record->size) != sizeof(*record) ||
	    record->mode > BOOT_MODE_UPDATE)
		return 0;

	if (record->mode == BOOT_MODE_NORMAL) {
		if (kernel_size || rootfs_size ||
		    !hash_is_zero(record->kernel_sha256) ||
		    !hash_is_zero(record->rootfs_sha256))
			return 0;
	} else if ((version == BOOTCTRL_VERSION_LEGACY &&
		    (!kernel_size || !rootfs_size)) ||
		   (version == BOOTCTRL_VERSION &&
		    !kernel_size && !rootfs_size) ||
		   !update_metadata_valid(kernel_size, KERNEL_MAX_SIZE,
					  record->kernel_sha256) ||
		   !update_metadata_valid(rootfs_size, ROOTFS_MAX_SIZE,
					  record->rootfs_sha256)) {
		return 0;
	}

	expected = crc32_bytes(record, offsetof(struct bootctrl_record, crc));
	return le32toh(record->crc) == expected;
}

static int sequence_after(uint32_t first, uint32_t second)
{
	return (int32_t)(first - second) > 0;
}

static int read_text_file(const char *path, char *buffer, size_t size)
{
	ssize_t length;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	length = read(fd, buffer, size - 1);
	if (length < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	buffer[length] = '\0';
	buffer[strcspn(buffer, "\r\n")] = '\0';
	return 0;
}

static void bootctrl_close(struct bootctrl_mtd *mtd)
{
	int saved_errno = errno;

	free(mtd->page);
	if (mtd->fd >= 0)
		close(mtd->fd);
	if (mtd->lib)
		libmtd_close(mtd->lib);
	errno = saved_errno;
}

static int bootctrl_open(struct bootctrl_mtd *mtd, int writable)
{
	char device[64];
	int flags;

	memset(mtd, 0, sizeof(*mtd));
	mtd->fd = -1;
	mtd->lib = libmtd_open();
	if (!mtd->lib) {
		if (!errno)
			errno = ENODEV;
		return -1;
	}
	if (mtd_get_dev_info2(mtd->lib, BOOTCTRL_MTD_NAME, &mtd->info))
		goto fail;
	if (mtd->info.eb_cnt < BOOTCTRL_SLOTS || mtd->info.eb_size <= 0 ||
	    mtd->info.min_io_size < (int)sizeof(struct bootctrl_record) ||
	    mtd->info.min_io_size > mtd->info.eb_size ||
	    (writable && !mtd->info.writable)) {
		errno = EINVAL;
		goto fail;
	}
	if (snprintf(device, sizeof(device), "/dev/mtd%d", mtd->info.mtd_num) >=
	    (int)sizeof(device)) {
		errno = ENAMETOOLONG;
		goto fail;
	}
	flags = writable ? O_RDWR | O_SYNC : O_RDONLY;
	mtd->fd = open(device, flags | O_CLOEXEC);
	if (mtd->fd < 0)
		goto fail;
	mtd->page = malloc((size_t)mtd->info.min_io_size);
	if (!mtd->page)
		goto fail;

	return 0;

fail:
	bootctrl_close(mtd);
	return -1;
}

static int read_record(struct bootctrl_mtd *mtd, int slot,
		       struct bootctrl_record *record)
{
	if (slot < 0 || slot >= BOOTCTRL_SLOTS) {
		errno = EINVAL;
		return -1;
	}
	if (mtd_read(&mtd->info, mtd->fd, slot, 0, mtd->page,
		     mtd->info.min_io_size))
		return -1;
	memcpy(record, mtd->page, sizeof(*record));
	if (!record_valid(record)) {
		errno = EBADMSG;
		return -1;
	}

	return 0;
}

static void state_from_record(struct boot_state *state,
			      const struct bootctrl_record *record, int source)
{
	state->sequence = le32toh(record->sequence);
	state->mode = record->mode;
	state->kernel_size = le32toh(record->kernel_size);
	state->rootfs_size = le32toh(record->rootfs_size);
	memcpy(state->kernel_sha256, record->kernel_sha256, SHA256_SIZE);
	memcpy(state->rootfs_sha256, record->rootfs_sha256, SHA256_SIZE);
	state->source = source;
}

static int load_state(struct boot_state *state)
{
	struct bootctrl_record record;
	struct bootctrl_mtd mtd;
	int good_slots = 0;
	int ret = -1;
	int slot;

	memset(state, 0, sizeof(*state));
	state->mode = BOOT_MODE_NORMAL;
	state->source = -1;
	if (bootctrl_open(&mtd, 0))
		return -1;

	for (slot = 0; slot < BOOTCTRL_SLOTS; slot++) {
		int bad = mtd_is_bad(&mtd.info, mtd.fd, slot);

		if (bad < 0)
			goto out;
		if (bad)
			continue;
		good_slots++;

		if (read_record(&mtd, slot, &record)) {
			if (errno == EBADMSG)
				continue;
			goto out;
		}
		if (state->source < 0 ||
		    sequence_after(le32toh(record.sequence), state->sequence))
			state_from_record(state, &record, slot);
	}

	if (!good_slots) {
		errno = ENOSPC;
		goto out;
	}
	ret = 0;

out:
	bootctrl_close(&mtd);
	return ret;
}

static void record_from_state(struct bootctrl_record *record,
			      const struct boot_state *state)
{
	memset(record, 0, sizeof(*record));
	record->magic = htole32(BOOTCTRL_MAGIC);
	record->version = htole16(BOOTCTRL_VERSION);
	record->size = htole16(sizeof(*record));
	record->sequence = htole32(state->sequence);
	record->mode = state->mode;
	record->kernel_size = htole32(state->kernel_size);
	record->rootfs_size = htole32(state->rootfs_size);
	memcpy(record->kernel_sha256, state->kernel_sha256, SHA256_SIZE);
	memcpy(record->rootfs_sha256, state->rootfs_sha256, SHA256_SIZE);
	record->crc = htole32(crc32_bytes(record,
					 offsetof(struct bootctrl_record, crc)));
}

static int store_state(struct boot_state *state)
{
	struct bootctrl_record verify, record;
	struct boot_state next = *state;
	struct bootctrl_mtd mtd;
	int ret = -1;
	int target = -1;
	int i;

	if (bootctrl_open(&mtd, 1))
		return -1;

	for (i = 0; i < BOOTCTRL_SLOTS; i++) {
		int candidate = state->source < 0 ? i :
			(state->source + i + 1) % BOOTCTRL_SLOTS;
		int bad = mtd_is_bad(&mtd.info, mtd.fd, candidate);

		if (bad < 0)
			goto out;
		if (!bad) {
			target = candidate;
			break;
		}
	}
	if (target < 0) {
		errno = ENOSPC;
		goto out;
	}

	next.sequence++;
	record_from_state(&record, &next);
	memset(mtd.page, 0xff, (size_t)mtd.info.min_io_size);
	memcpy(mtd.page, &record, sizeof(record));

	if (mtd_erase(mtd.lib, &mtd.info, mtd.fd, target))
		goto out;
	if (mtd_write(mtd.lib, &mtd.info, mtd.fd, target, 0, mtd.page,
		      mtd.info.min_io_size, NULL, 0, 0))
		goto out;
	if (read_record(&mtd, target, &verify))
		goto out;
	if (memcmp(&verify, &record, sizeof(record))) {
		errno = EIO;
		goto out;
	}

	state->sequence = next.sequence;
	state->source = target;
	ret = 0;

out:
	bootctrl_close(&mtd);
	return ret;
}

static const char *mode_name(uint8_t mode)
{
	return mode == BOOT_MODE_UPDATE ? "update" : "normal";
}

static int current_mode(uint8_t *mode)
{
	char command_line[4096];
	char *saveptr = NULL;
	char *token;

	if (read_text_file("/proc/cmdline", command_line, sizeof(command_line)))
		return -1;
	for (token = strtok_r(command_line, " ", &saveptr); token;
	     token = strtok_r(NULL, " ", &saveptr)) {
		if (!strcmp(token, "ota.mode=normal")) {
			*mode = BOOT_MODE_NORMAL;
			return 0;
		}
	}

	errno = ENODATA;
	return -1;
}

static int parse_size(const char *text, uint32_t maximum, uint32_t *value)
{
	unsigned long long parsed;
	char *end;

	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end || parsed > maximum) {
		errno = EINVAL;
		return -1;
	}
	*value = (uint32_t)parsed;
	return 0;
}

static int hex_value(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

static int parse_sha256(const char *text, uint8_t hash[SHA256_SIZE])
{
	size_t i;

	if (!strcmp(text, "-")) {
		memset(hash, 0, SHA256_SIZE);
		return 0;
	}

	if (strlen(text) != SHA256_SIZE * 2) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < SHA256_SIZE; i++) {
		int high = hex_value(text[i * 2]);
		int low = hex_value(text[i * 2 + 1]);

		if (high < 0 || low < 0) {
			errno = EINVAL;
			return -1;
		}
		hash[i] = (uint8_t)(high << 4 | low);
	}
	if (hash_is_zero(hash)) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static void clear_update(struct boot_state *state)
{
	state->mode = BOOT_MODE_NORMAL;
	state->kernel_size = 0;
	state->rootfs_size = 0;
	memset(state->kernel_sha256, 0, SHA256_SIZE);
	memset(state->rootfs_sha256, 0, SHA256_SIZE);
}

static void print_status(const struct boot_state *state)
{
	uint8_t current;
	char source[16];
	const char *current_name = "unknown";
	const char *targets = "none";

	if (!current_mode(&current))
		current_name = mode_name(current);
	if (state->kernel_size && state->rootfs_size)
		targets = "kernel,rootfs";
	else if (state->kernel_size)
		targets = "kernel";
	else if (state->rootfs_size)
		targets = "rootfs";
	if (state->source < 0)
		strcpy(source, "defaults");
	else
		snprintf(source, sizeof(source), "slot%d", state->source);
	printf("mode=%s targets=%s sequence=%" PRIu32 " current=%s source=%s "
	       "kernel_size=%" PRIu32 " rootfs_size=%" PRIu32 " "
	       "kernel_limit=%u rootfs_limit=%u\n",
	       mode_name(state->mode), targets, state->sequence, current_name,
	       source,
	       state->kernel_size, state->rootfs_size,
	       KERNEL_MAX_SIZE, ROOTFS_MAX_SIZE);
}

static void usage(FILE *stream)
{
	fprintf(stream,
		"Usage:\n"
		"  epass-otactl status\n"
		"  epass-otactl request-update KERNEL_SIZE KERNEL_SHA256 "
		"ROOTFS_SIZE ROOTFS_SHA256\n"
		"  epass-otactl cancel-update\n"
		"Use '0 -' for an image that is not part of the update.\n");
}

int main(int argc, char **argv)
{
	struct boot_state state;
	uint8_t current;
	int lock_fd;

	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	lock_fd = open("/run/epass-otactl.lock", O_CREAT | O_RDWR | O_CLOEXEC,
		       0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX)) {
		perror("epass-otactl: lock");
		return 1;
	}
	if (load_state(&state)) {
		perror("epass-otactl: read raw boot control slots");
		return 1;
	}

	if (!strcmp(argv[1], "status") && argc == 2) {
		print_status(&state);
		return 0;
	}
	if (!strcmp(argv[1], "request-update") && argc == 6) {
		if (current_mode(&current) || current != BOOT_MODE_NORMAL) {
			fprintf(stderr, "epass-otactl: not running in normal mode\n");
			return 1;
		}
		if (state.mode != BOOT_MODE_NORMAL) {
			fprintf(stderr, "epass-otactl: an update is already pending\n");
			return 1;
		}
		if (parse_size(argv[2], KERNEL_MAX_SIZE, &state.kernel_size) ||
		    parse_sha256(argv[3], state.kernel_sha256) ||
		    parse_size(argv[4], ROOTFS_MAX_SIZE, &state.rootfs_size) ||
		    parse_sha256(argv[5], state.rootfs_sha256) ||
		    (!state.kernel_size && !state.rootfs_size) ||
		    !update_metadata_valid(state.kernel_size, KERNEL_MAX_SIZE,
					   state.kernel_sha256) ||
		    !update_metadata_valid(state.rootfs_size, ROOTFS_MAX_SIZE,
					   state.rootfs_sha256)) {
			fprintf(stderr, "epass-otactl: invalid update metadata\n");
			return 2;
		}
		state.mode = BOOT_MODE_UPDATE;
		if (store_state(&state)) {
			perror("epass-otactl: request update");
			return 1;
		}
		print_status(&state);
		return 0;
	}
	if (!strcmp(argv[1], "cancel-update") && argc == 2) {
		if (current_mode(&current) || current != BOOT_MODE_NORMAL) {
			fprintf(stderr, "epass-otactl: not running in normal mode\n");
			return 1;
		}
		if (state.mode == BOOT_MODE_UPDATE) {
			clear_update(&state);
			if (store_state(&state)) {
				perror("epass-otactl: cancel update");
				return 1;
			}
		}
		print_status(&state);
		return 0;
	}

	usage(stderr);
	return 2;
}
