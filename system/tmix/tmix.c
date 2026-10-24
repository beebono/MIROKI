/*
 * tmix: a minimal tinymix on the raw ALSA control interface, no alsa-lib.
 *
 * Unlike amixer (and UCM's cset), integers go to the driver exactly as
 * given. alsa-lib clamps them to the control's advertised range, and the
 * VBC driver's "DSP VBC Profile Select" advertises a max of 0x0fffffff,
 * below the values stock Android writes (961216512, 0x394b0000): through
 * alsa-lib those silently become 0x0fffffff.
 *
 *   tmix [-c CARD]                 list the controls
 *   tmix [-c CARD] NAME            show NAME's value(s) and range or items
 *   tmix [-c CARD] NAME V [V ...]  set NAME, then print what it reads back
 *
 * V is a number (decimal or 0x hex), on/off for a switch, or an enum item
 * name (or index). Values may also be comma separated ("63,63"); a single
 * value fills every channel.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

static int ctl_fd;

static const char *type_name(int type)
{
	switch (type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:	return "bool";
	case SNDRV_CTL_ELEM_TYPE_INTEGER:	return "int";
	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:	return "enum";
	case SNDRV_CTL_ELEM_TYPE_BYTES:		return "bytes";
	case SNDRV_CTL_ELEM_TYPE_IEC958:	return "iec958";
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:	return "int64";
	default:				return "?";
	}
}

/* All element ids on the card; the caller frees. */
static struct snd_ctl_elem_id *list_ids(unsigned int *n)
{
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids;

	memset(&list, 0, sizeof(list));
	if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		return NULL;
	ids = calloc(list.count ? list.count : 1, sizeof(*ids));
	if (!ids)
		return NULL;
	list.space = list.count;
	list.pids = ids;
	if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
		free(ids);
		return NULL;
	}
	*n = list.used;
	return ids;
}

static int enum_item_name(struct snd_ctl_elem_info *info, unsigned int item,
			  char *name, size_t len)
{
	struct snd_ctl_elem_info ei = *info;

	ei.value.enumerated.item = item;
	if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei) < 0)
		return -1;
	snprintf(name, len, "%s", ei.value.enumerated.name);
	return 0;
}

static int show(struct snd_ctl_elem_info *info)
{
	struct snd_ctl_elem_value val;
	char item[64];

	memset(&val, 0, sizeof(val));
	val.id = info->id;
	if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
		fprintf(stderr, "tmix: read '%s': %s\n", info->id.name, strerror(errno));
		return -1;
	}

	printf("'%s' =", info->id.name);
	for (unsigned int i = 0; i < info->count; i++) {
		switch (info->type) {
		case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
			printf(" %s", val.value.integer.value[i] ? "on" : "off");
			break;
		case SNDRV_CTL_ELEM_TYPE_INTEGER:
			printf(" %ld", val.value.integer.value[i]);
			break;
		case SNDRV_CTL_ELEM_TYPE_INTEGER64:
			printf(" %lld", val.value.integer64.value[i]);
			break;
		case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
			if (enum_item_name(info, val.value.enumerated.item[i], item, sizeof(item)))
				printf(" #%u", val.value.enumerated.item[i]);
			else
				printf(" %s", item);
			break;
		default:
			printf(" (%s)", type_name(info->type));
			break;
		}
	}

	if (info->type == SNDRV_CTL_ELEM_TYPE_INTEGER)
		printf("  [%ld..%ld]", info->value.integer.min, info->value.integer.max);
	else if (info->type == SNDRV_CTL_ELEM_TYPE_INTEGER64)
		printf("  [%lld..%lld]", info->value.integer64.min, info->value.integer64.max);
	else if (info->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		printf("  {");
		for (unsigned int i = 0; i < info->value.enumerated.items; i++)
			if (!enum_item_name(info, i, item, sizeof(item)))
				printf("%s%s", i ? ", " : "", item);
		printf("}");
	}
	printf("\n");
	return 0;
}

/* Parse one value for the control's type into slot i of val. */
static int parse_value(struct snd_ctl_elem_info *info, const char *s,
		       struct snd_ctl_elem_value *val, unsigned int i)
{
	char *end, item[64];
	long long v;

	switch (info->type) {
	case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
		if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcmp(s, "1"))
			val->value.integer.value[i] = 1;
		else if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcmp(s, "0"))
			val->value.integer.value[i] = 0;
		else
			return -1;
		return 0;

	case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
		for (unsigned int n = 0; n < info->value.enumerated.items; n++) {
			if (!enum_item_name(info, n, item, sizeof(item)) && !strcmp(s, item)) {
				val->value.enumerated.item[i] = n;
				return 0;
			}
		}
		errno = 0;
		v = strtoll(s, &end, 0);
		if (errno || *end || v < 0 || v >= info->value.enumerated.items)
			return -1;
		val->value.enumerated.item[i] = v;
		return 0;

	case SNDRV_CTL_ELEM_TYPE_INTEGER:
	case SNDRV_CTL_ELEM_TYPE_INTEGER64:
		errno = 0;
		v = strtoll(s, &end, 0);
		if (errno || *end)
			return -1;
		if (info->type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
			if (v < info->value.integer.min || v > info->value.integer.max)
				fprintf(stderr, "tmix: '%s': %lld is outside [%ld..%ld], writing it as is\n",
					info->id.name, v, info->value.integer.min,
					info->value.integer.max);
			val->value.integer.value[i] = v;
		} else {
			val->value.integer64.value[i] = v;
		}
		return 0;

	default:
		return -1;
	}
}

static int set(struct snd_ctl_elem_info *info, int argc, char **argv)
{
	struct snd_ctl_elem_value val;
	char *vals[512], *p;
	unsigned int nv = 0;

	/* "63,63" and "63 63" are the same */
	for (int a = 0; a < argc; a++)
		for (p = strtok(argv[a], ","); p && nv < 512; p = strtok(NULL, ","))
			vals[nv++] = p;
	if (!nv)
		return -1;

	memset(&val, 0, sizeof(val));
	val.id = info->id;
	for (unsigned int i = 0; i < info->count; i++) {
		const char *s = vals[i < nv ? i : nv - 1];

		if (parse_value(info, s, &val, i)) {
			fprintf(stderr, "tmix: '%s' (%s): bad value '%s'\n",
				info->id.name, type_name(info->type), s);
			return -1;
		}
	}

	if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
		fprintf(stderr, "tmix: write '%s': %s\n", info->id.name, strerror(errno));
		return -1;
	}
	return show(info);
}

int main(int argc, char **argv)
{
	struct snd_ctl_elem_id *ids;
	struct snd_ctl_elem_info info;
	unsigned int n, i;
	char path[32];
	int card = 0, ret;

	if (argc > 2 && !strcmp(argv[1], "-c")) {
		card = atoi(argv[2]);
		argc -= 2;
		argv += 2;
	}

	snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
	ctl_fd = open(path, O_RDWR | O_CLOEXEC);
	if (ctl_fd < 0) {
		fprintf(stderr, "tmix: %s: %s\n", path, strerror(errno));
		return 1;
	}
	ids = list_ids(&n);
	if (!ids) {
		fprintf(stderr, "tmix: listing controls: %s\n", strerror(errno));
		return 1;
	}

	if (argc == 1) {
		for (i = 0; i < n; i++) {
			memset(&info, 0, sizeof(info));
			info.id = ids[i];
			if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
				continue;
			printf("#%u\t%s x%u\t%s\n", ids[i].numid, type_name(info.type),
			       info.count, ids[i].name);
		}
		return 0;
	}

	for (i = 0; i < n; i++)
		if (!strcmp((const char *)ids[i].name, argv[1]))
			break;
	if (i == n) {
		fprintf(stderr, "tmix: no control '%s'\n", argv[1]);
		return 1;
	}

	memset(&info, 0, sizeof(info));
	info.id = ids[i];
	if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
		fprintf(stderr, "tmix: info '%s': %s\n", argv[1], strerror(errno));
		return 1;
	}

	ret = argc == 2 ? show(&info) : set(&info, argc - 2, argv + 2);
	return ret ? 1 : 0;
}
