#ifndef STUB_DIRENT_H
#define STUB_DIRENT_H
/* Minimal host stub so scan_dir() compiles on the test host. */
struct dirent {
    const char *d_name;
    unsigned char d_type;
};
#define DT_DIR 4
typedef struct DIR DIR;
static inline DIR *opendir(const char *p) { (void)p; return NULL; }
static inline int readdir_r(DIR *d, struct dirent *e, struct dirent **r) {
    (void)d; (void)e; *r = NULL; return 0;
}
static inline void closedir(DIR *d) { (void)d; }
static inline struct dirent *readdir(DIR *d) { (void)d; return NULL; }
#endif
