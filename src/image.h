#ifndef IMAGE_H
#define IMAGE_H

#define IMAGE_NAME_LEN  64
#define IMAGE_TAG_LEN   32
#define IMAGE_PATH_LEN  512
/* max length of a "name:tag" reference string */
#define IMAGE_REF_LEN   (IMAGE_NAME_LEN + IMAGE_TAG_LEN + 2)

typedef struct {
    char name[IMAGE_NAME_LEN];
    char tag[IMAGE_TAG_LEN];
    char rootfs[IMAGE_PATH_LEN];
    long created_at;
} ImageRecord;

/* Register an existing bootstrapped rootfs directory as a named image.
 * If tag is NULL or empty, defaults to "latest". */
int image_build(const char *name, const char *tag, const char *rootfs_path);

/* Create an alias dst_ref pointing at the same rootfs as src_ref. */
int image_tag(const char *src_ref, const char *dst_ref);

/* Resolve an image reference ("name" or "name:tag") to its rootfs path.
 * Returns 0 and writes the path into out_path on success, -1 on not-found. */
int image_resolve(const char *ref, char *out_path, int out_size);

/* Print all registered images. */
int image_list(void);

/* Print detailed info about one image. */
int image_inspect(const char *ref);

/* Remove an image record (does not delete the rootfs directory). */
int image_remove(const char *name, const char *tag);

#endif
