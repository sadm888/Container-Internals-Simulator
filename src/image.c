#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "image.h"

#define IMAGE_REGISTRY     "images.meta"
#define IMAGE_REGISTRY_TMP "images.meta.tmp"
#define DEFAULT_TAG        "latest"

/* Split "name:tag" into separate buffers.
 * Empty tag after colon (e.g. "myapp:") is treated as "latest". */
static void parse_ref(const char *ref, char *name, int name_size,
                      char *tag, int tag_size) {
    const char *colon = strchr(ref, ':');

    if (colon != NULL) {
        int nlen = (int)(colon - ref);
        if (nlen >= name_size) {
            nlen = name_size - 1;
        }
        memcpy(name, ref, (size_t)nlen);
        name[nlen] = '\0';

        /* An empty tag after the colon (e.g. "myapp:") defaults to "latest". */
        if (*(colon + 1) != '\0') {
            snprintf(tag, tag_size, "%s", colon + 1);
        } else {
            snprintf(tag, tag_size, "%s", DEFAULT_TAG);
        }
    } else {
        snprintf(name, name_size, "%s", ref);
        snprintf(tag,  tag_size,  "%s", DEFAULT_TAG);
    }
}

static int load_registry(ImageRecord *records, int max_records) {
    FILE *f;
    char line[1024];
    int count = 0;

    f = fopen(IMAGE_REGISTRY, "r");
    if (f == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), f) != NULL && count < max_records) {
        char *fields[4];
        char *tok;
        int idx = 0;

        line[strcspn(line, "\r\n")] = '\0';
        tok = strtok(line, "\t");
        while (tok != NULL && idx < 4) {
            fields[idx++] = tok;
            tok = strtok(NULL, "\t");
        }
        if (idx < 4) {
            continue;
        }

        snprintf(records[count].name,   sizeof(records[count].name),   "%s", fields[0]);
        snprintf(records[count].tag,    sizeof(records[count].tag),     "%s", fields[1]);
        snprintf(records[count].rootfs, sizeof(records[count].rootfs),  "%s", fields[2]);
        records[count].created_at = atol(fields[3]);
        count++;
    }

    fclose(f);
    return count;
}

static int save_registry(const ImageRecord *records, int count) {
    FILE *f;
    int i;

    f = fopen(IMAGE_REGISTRY_TMP, "w");
    if (f == NULL) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (fprintf(f, "%s\t%s\t%s\t%ld\n",
                    records[i].name, records[i].tag,
                    records[i].rootfs, records[i].created_at) < 0) {
            fclose(f);
            return -1;
        }
    }

    if (fclose(f) != 0) {
        return -1;
    }

    return rename(IMAGE_REGISTRY_TMP, IMAGE_REGISTRY);
}

/* Find a record matching name+tag. Returns index into records[], or -1. */
static int find_record(ImageRecord *records, int count,
                       const char *name, const char *tag) {
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(records[i].name, name) == 0 &&
            strcmp(records[i].tag,  tag)  == 0) {
            return i;
        }
    }
    return -1;
}

int image_build(const char *name, const char *tag, const char *rootfs_path) {
    ImageRecord records[256];
    int count;
    int idx;
    char effective_tag[IMAGE_TAG_LEN];
    char abs_path[IMAGE_PATH_LEN];

    if (name == NULL || name[0] == '\0' ||
        rootfs_path == NULL || rootfs_path[0] == '\0') {
        printf("[error] usage: image build <name>[:<tag>] <rootfs-path>\n\n");
        return -1;
    }

    snprintf(effective_tag, sizeof(effective_tag), "%s",
             (tag != NULL && tag[0] != '\0') ? tag : DEFAULT_TAG);

    if (realpath(rootfs_path, abs_path) == NULL) {
        printf("[error] rootfs path not found: %s\n\n", rootfs_path);
        return -1;
    }

    count = load_registry(records, 256);
    idx   = find_record(records, count, name, effective_tag);

    if (idx >= 0) {
        snprintf(records[idx].rootfs, sizeof(records[idx].rootfs), "%s", abs_path);
        records[idx].created_at = (long)time(NULL);
        if (save_registry(records, count) != 0) {
            printf("[error] failed to save image registry\n\n");
            return -1;
        }
        printf("[image] updated %s:%s -> %s\n\n", name, effective_tag, abs_path);
        return 0;
    }

    if (count >= 256) {
        printf("[error] image registry full\n\n");
        return -1;
    }

    snprintf(records[count].name,   sizeof(records[count].name),   "%s", name);
    snprintf(records[count].tag,    sizeof(records[count].tag),     "%s", effective_tag);
    snprintf(records[count].rootfs, sizeof(records[count].rootfs),  "%s", abs_path);
    records[count].created_at = (long)time(NULL);
    count++;

    if (save_registry(records, count) != 0) {
        printf("[error] failed to save image registry\n\n");
        return -1;
    }

    printf("[image] registered %s:%s -> %s\n\n", name, effective_tag, abs_path);
    return 0;
}

int image_tag(const char *src_ref, const char *dst_ref) {
    char src_path[IMAGE_PATH_LEN];
    char dst_name[IMAGE_NAME_LEN];
    char dst_tag[IMAGE_TAG_LEN];

    if (src_ref == NULL || src_ref[0] == '\0' ||
        dst_ref == NULL || dst_ref[0] == '\0') {
        printf("[error] usage: image tag <src>[:<tag>] <dst>[:<tag>]\n\n");
        return -1;
    }

    if (image_resolve(src_ref, src_path, sizeof(src_path)) != 0) {
        printf("[error] image not found: %s\n\n", src_ref);
        return -1;
    }

    parse_ref(dst_ref, dst_name, sizeof(dst_name), dst_tag, sizeof(dst_tag));
    return image_build(dst_name, dst_tag, src_path);
}

int image_resolve(const char *ref, char *out_path, int out_size) {
    ImageRecord records[256];
    int count;
    int idx;
    char name[IMAGE_NAME_LEN];
    char tag[IMAGE_TAG_LEN];

    if (ref == NULL || out_path == NULL || out_size <= 0) {
        errno = EINVAL;
        return -1;
    }

    parse_ref(ref, name, sizeof(name), tag, sizeof(tag));
    count = load_registry(records, 256);
    idx   = find_record(records, count, name, tag);

    if (idx < 0) {
        errno = ENOENT;
        return -1;
    }

    snprintf(out_path, (size_t)out_size, "%s", records[idx].rootfs);
    return 0;
}

int image_list(void) {
    ImageRecord records[256];
    int count;
    int i;

    count = load_registry(records, 256);

    printf("\n");
    printf("%-32s %-20s %s\n", "IMAGE", "CREATED", "ROOTFS");
    printf("--------------------------------------------------------------------------------\n");

    for (i = 0; i < count; i++) {
        char combined[IMAGE_REF_LEN];
        char time_buf[32];
        time_t ts = (time_t)records[i].created_at;
        struct tm *tm_info = localtime(&ts);

        if (tm_info != NULL) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm_info);
        } else {
            snprintf(time_buf, sizeof(time_buf), "unknown");
        }

        snprintf(combined, sizeof(combined), "%s:%s",
                 records[i].name, records[i].tag);
        printf("%-32s %-20s %s\n", combined, time_buf, records[i].rootfs);
    }

    if (count == 0) {
        printf("no images registered\n");
    }
    printf("\n");
    return 0;
}

int image_inspect(const char *ref) {
    ImageRecord records[256];
    int count;
    int idx;
    char name[IMAGE_NAME_LEN];
    char tag[IMAGE_TAG_LEN];
    char time_buf[32];
    time_t ts;
    struct tm *tm_info;

    if (ref == NULL || ref[0] == '\0') {
        printf("[error] usage: image inspect <name>[:<tag>]\n\n");
        return -1;
    }

    parse_ref(ref, name, sizeof(name), tag, sizeof(tag));
    count = load_registry(records, 256);
    idx   = find_record(records, count, name, tag);

    if (idx < 0) {
        printf("[error] image not found: %s\n\n", ref);
        return -1;
    }

    ts      = (time_t)records[idx].created_at;
    tm_info = localtime(&ts);
    if (tm_info != NULL) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", tm_info);
    } else {
        snprintf(time_buf, sizeof(time_buf), "unknown");
    }

    printf("{\n");
    printf("  \"Name\"    : \"%s\",\n", records[idx].name);
    printf("  \"Tag\"     : \"%s\",\n", records[idx].tag);
    printf("  \"Ref\"     : \"%s:%s\",\n", records[idx].name, records[idx].tag);
    printf("  \"Rootfs\"  : \"%s\",\n", records[idx].rootfs);
    printf("  \"Created\" : \"%s\"\n", time_buf);
    printf("}\n\n");
    return 0;
}

int image_remove(const char *name, const char *tag) {
    ImageRecord records[256];
    int count;
    int found = 0;
    char effective_tag[IMAGE_TAG_LEN];
    int write_count = 0;
    ImageRecord out[256];
    int i;

    if (name == NULL || name[0] == '\0') {
        printf("[error] usage: image rm <name>[:<tag>]\n\n");
        return -1;
    }

    snprintf(effective_tag, sizeof(effective_tag), "%s",
             (tag != NULL && tag[0] != '\0') ? tag : DEFAULT_TAG);

    count = load_registry(records, 256);

    for (i = 0; i < count; i++) {
        if (strcmp(records[i].name, name) == 0 &&
            strcmp(records[i].tag,  effective_tag) == 0) {
            found = 1;
            continue;
        }
        out[write_count++] = records[i];
    }

    if (!found) {
        printf("[error] image %s:%s not found\n\n", name, effective_tag);
        return -1;
    }

    if (save_registry(out, write_count) != 0) {
        printf("[error] failed to update image registry\n\n");
        return -1;
    }

    printf("[image] removed %s:%s\n\n", name, effective_tag);
    return 0;
}
