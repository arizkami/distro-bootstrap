/*
 * bootstrap.c - Multi-distro pivot_root bootstrap
 *
 * Build:
 * gcc -static -O2 -o bootstrap bootstrap.c
 *
 * Usage:
 * bootstrap --custom /path/to/rootfs_folder
 * bootstrap --custom /dev/sda3
 * bootstrap --custom /path/to/system.img
 * bootstrap --custom /path/to/system.squashfs
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

#define LOG(fmt, ...)  fprintf(stdout, "[bootstrap] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) fprintf(stderr, "[bootstrap] WARN: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "[bootstrap] ERROR: " fmt "\n", ##__VA_ARGS__)

static inline int pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

static int mkdirp(const char *path, mode_t mode)
{
    char   tmp[4096];
    char  *p;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                WARN("mkdir %s: %s", tmp, strerror(errno));
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        if (errno != EEXIST)
            WARN("mkdir %s: %s", tmp, strerror(errno));
    }
    return 0;
}

static void do_mount_fs(const char *fstype, const char *src, const char *dst,
                        unsigned long flags, const char *opts)
{
    mkdirp(dst, 0755);
    if (mount(src, dst, fstype, flags, opts) != 0)
        WARN("mount -t %s %s -> %s: %s", fstype, src, dst, strerror(errno));
    else
        LOG("  mount %-12s -> %s", fstype, dst);
}

static void do_mount_bind(const char *src, const char *dst)
{
    mkdirp(dst, 0755);
    if (mount(src, dst, NULL, MS_BIND, NULL) != 0)
        WARN("bind  %s -> %s: %s", src, dst, strerror(errno));
    else
        LOG("  bind  %s -> %s", src, dst);
}

/* Mount a block device natively by guessing the filesystem */
static int mount_block_device(const char *dev, const char *mnt)
{
    mkdirp(mnt, 0755);
    /* Added squashfs to the detection list */
    static const char *fs_types[] = {"ext4", "xfs", "btrfs", "ext3", "ext2", "vfat", "squashfs", NULL};
    
    for (int i = 0; fs_types[i] != NULL; i++) {
        if (mount(dev, mnt, fs_types[i], 0, NULL) == 0) {
            LOG("  mount block device %s as %s -> %s", dev, fs_types[i], mnt);
            return 0;
        }
    }
    WARN("failed to mount %s (tried ext4, xfs, btrfs, squashfs, etc.): %s", dev, strerror(errno));
    return -1;
}

/* Mount a regular file (img/squashfs) by outsourcing to the host's mount utility for loop handling */
static int mount_file_image(const char *file, const char *mnt)
{
    mkdirp(mnt, 0755);
    char cmd[4096];
    
    /* Using the standard mount tool handles loop allocation and filesystem detection automatically */
    snprintf(cmd, sizeof(cmd), "mount -o loop \"%s\" \"%s\"", file, mnt);
    LOG("  loop mounting file: %s", file);
    
    int ret = system(cmd);
    if (ret != 0) {
        WARN("failed to loop mount image file: %s", file);
        return -1;
    }
    return 0;
}

static void do_umount_lazy(const char *path)
{
    if (umount2(path, MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT)
        WARN("umount -l %s: %s", path, strerror(errno));
}

static const char *find_exec(const char *candidates[], int n)
{
    struct stat st;
    for (int i = 0; i < n; i++) {
        if (stat(candidates[i], &st) == 0 && (st.st_mode & S_IXUSR))
            return candidates[i];
    }
    return NULL;
}

static void safe_symlink(const char *target, const char *linkpath)
{
    struct stat st;
    if (lstat(linkpath, &st) == 0) {
        if (S_ISLNK(st.st_mode))
            return; 
        remove(linkpath); 
    }
    if (symlink(target, linkpath) != 0)
        WARN("symlink %s -> %s: %s", target, linkpath, strerror(errno));
    else
        LOG("  symlink %s -> %s", target, linkpath);
}

/* ------------------------------------------------------------------ */
/* Distro descriptor                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    DISTRO_ARCHLINUX,
    DISTRO_FEDORA,
    DISTRO_UBUNTU,
    DISTRO_ALPINE,
    DISTRO_GENTOO,
    DISTRO_RHEL,
    DISTRO_CUSTOM,
} Distro;

static char custom_path_global[4096] = {0};

static const char *distro_name(Distro d)
{
    switch (d) {
    case DISTRO_ARCHLINUX: return "archlinux";
    case DISTRO_FEDORA:    return "fedora";
    case DISTRO_UBUNTU:    return "ubuntu";
    case DISTRO_ALPINE:    return "alpine";
    case DISTRO_GENTOO:    return "gentoo";
    case DISTRO_RHEL:      return "rhel";
    case DISTRO_CUSTOM:    return "custom";
    }
    return "unknown";
}

static const char *distro_target(Distro d)
{
    switch (d) {
    case DISTRO_ARCHLINUX: return "/archlinux";
    case DISTRO_FEDORA:    return "/fedora";
    case DISTRO_UBUNTU:    return "/ubuntu";
    case DISTRO_ALPINE:    return "/alpine";
    case DISTRO_GENTOO:    return "/gentoo";
    case DISTRO_RHEL:      return "/rhel";
    case DISTRO_CUSTOM:    return custom_path_global;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Phases                                                             */
/* ------------------------------------------------------------------ */

static void mount_host_api(Distro distro)
{
    LOG("mounting host API filesystems...");
    do_mount_fs("proc",      "proc",   "/proc",            0, NULL);
    do_mount_fs("sysfs",     "sys",    "/sys",             0, NULL);
    do_mount_fs("devtmpfs",  "dev",    "/dev",             0, NULL);
    do_mount_fs("devpts",    "devpts", "/dev/pts",         0, "gid=5,mode=620");
    do_mount_fs("tmpfs",     "tmpfs",  "/run",             0, "mode=755");
    do_mount_fs("tmpfs",     "tmpfs",  "/tmp",             0, "mode=1777");
    do_mount_fs("cgroup2",   "none",   "/sys/fs/cgroup",   0, NULL);

    if (distro == DISTRO_FEDORA || distro == DISTRO_RHEL || distro == DISTRO_CUSTOM) {
        do_mount_fs("tmpfs", "tmpfs", "/sys/fs/cgroup/unified", 0, NULL);
    }
}

static void prepare_target_mounts(const char *target, Distro distro)
{
    LOG("preparing mount points inside %s...", target);

    static const char *common[] = {
        "proc", "sys", "dev", "dev/pts", "dev/shm",
        "run", "run/lock", "tmp",
        "sys/fs/cgroup", "sys/kernel/debug", "sys/kernel/tracing",
    };
    char path[4096];
    for (size_t i = 0; i < sizeof(common)/sizeof(*common); i++) {
        snprintf(path, sizeof(path), "%s/%s", target, common[i]);
        mkdirp(path, 0755);
    }

    switch (distro) {
    case DISTRO_FEDORA:
    case DISTRO_RHEL:
    case DISTRO_CUSTOM:
        snprintf(path, sizeof(path), "%s/sys/fs/cgroup/unified", target); mkdirp(path, 0755);
        snprintf(path, sizeof(path), "%s/var/run",  target); mkdirp(path, 0755);
        snprintf(path, sizeof(path), "%s/var/lock", target); mkdirp(path, 0755);
        break;
    case DISTRO_UBUNTU:
        snprintf(path, sizeof(path), "%s/var/run",  target); mkdirp(path, 0755);
        snprintf(path, sizeof(path), "%s/run/shm",  target); mkdirp(path, 0755);
        break;
    case DISTRO_GENTOO:
        snprintf(path, sizeof(path), "%s/var/run", target); mkdirp(path, 0755);
        snprintf(path, sizeof(path), "%s/run/lock", target); mkdirp(path, 0755);
        break;
    case DISTRO_ALPINE:
        snprintf(path, sizeof(path), "%s/dev/shm",  target); mkdirp(path, 0755);
        break;
    case DISTRO_ARCHLINUX:
        snprintf(path, sizeof(path), "%s/sys/firmware/efi/efivars", target);
        mkdirp(path, 0755);
        break;
    }
}

static void do_pivot(const char *target, Distro distro)
{
    LOG("binding %s -> /mnt...", target);
    mkdirp("/mnt", 0755);
    if (mount(target, "/mnt", NULL, MS_BIND, NULL) != 0) {
        ERR("bind %s -> /mnt: %s", target, strerror(errno));
        execl("/bin/sh", "/bin/sh", NULL);
        _exit(1);
    }

    if (chdir("/mnt") != 0) {
        ERR("chdir /mnt: %s", strerror(errno));
        _exit(1);
    }

    mkdirp("bootstrap", 0755);

    LOG("binding API filesystems into new root...");
    static const char *api_fs[] = {
        "proc", "sys", "dev", "dev/pts", "run", "tmp", "sys/fs/cgroup",
    };
    char src[4096];
    for (size_t i = 0; i < sizeof(api_fs)/sizeof(*api_fs); i++) {
        snprintf(src, sizeof(src), "/%s", api_fs[i]);
        mkdirp(api_fs[i], 0755);
        do_mount_bind(src, api_fs[i]);
    }

    if (distro == DISTRO_UBUNTU || distro == DISTRO_ALPINE || distro == DISTRO_GENTOO) {
        mkdirp("dev/shm", 0755);
        if (mount("/dev/shm", "dev/shm", NULL, MS_BIND, NULL) != 0)
            do_mount_fs("tmpfs", "tmpfs", "dev/shm", 0, "mode=1777");
    }

    LOG("pivot_root...");
    if (pivot_root(".", "bootstrap") != 0) {
        ERR("pivot_root: %s", strerror(errno));
        _exit(1);
    }

    if (chdir("/") != 0) {
        ERR("chdir /: %s", strerror(errno));
        _exit(1);
    }
}

static void cleanup_old_root(void)
{
    LOG("cleaning up old root...");
    static const char *old[] = {
        "/bootstrap/sys/fs/cgroup", "/bootstrap/dev/pts", "/bootstrap/dev/shm",
        "/bootstrap/dev", "/bootstrap/proc", "/bootstrap/sys",
        "/bootstrap/run", "/bootstrap/tmp", "/bootstrap",
    };
    for (size_t i = 0; i < sizeof(old)/sizeof(*old); i++)
        do_umount_lazy(old[i]);
}

static void post_pivot_fixups(Distro distro)
{
    switch (distro) {
    case DISTRO_FEDORA:
    case DISTRO_RHEL:
    case DISTRO_CUSTOM:
        safe_symlink("/run",      "/var/run");
        safe_symlink("/run/lock", "/var/lock");
        break;
    case DISTRO_UBUNTU:
        safe_symlink("/run",     "/var/run");
        safe_symlink("/dev/shm", "/run/shm");
        break;
    case DISTRO_GENTOO:
        safe_symlink("/run", "/var/run");
        if (mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777") != 0 && errno != EBUSY)
            WARN("tmpfs on /dev/shm: %s", strerror(errno));
        break;
    case DISTRO_ALPINE:
        if (mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777") != 0 && errno != EBUSY)
            WARN("tmpfs on /dev/shm: %s", strerror(errno));
        break;
    case DISTRO_ARCHLINUX:
        break;
    }
}

static const char *find_init(Distro distro)
{
    static const char *systemd_first[] = {
        "/lib/systemd/systemd", "/usr/lib/systemd/systemd", "/sbin/init", "/bin/init",
    };
    static const char *arch_inits[] = {
        "/usr/lib/systemd/systemd", "/sbin/init", "/bin/init",
    };
    static const char *alpine_inits[] = {
        "/sbin/init", "/bin/busybox", "/bin/sh",
    };
    static const char *gentoo_inits[] = {
        "/sbin/init", "/lib/systemd/systemd", "/usr/lib/systemd/systemd", "/bin/sh",
    };
    static const char *custom_inits[] = {
        "/sbin/init", "/lib/systemd/systemd", "/usr/lib/systemd/systemd",
        "/bin/init", "/bin/busybox", "/bin/sh",
    };

    switch (distro) {
    case DISTRO_FEDORA:
    case DISTRO_RHEL:
    case DISTRO_UBUNTU:
        return find_exec(systemd_first, (int)(sizeof(systemd_first)/sizeof(*systemd_first)));
    case DISTRO_ARCHLINUX:
        return find_exec(arch_inits, (int)(sizeof(arch_inits)/sizeof(*arch_inits)));
    case DISTRO_ALPINE:
        return find_exec(alpine_inits, (int)(sizeof(alpine_inits)/sizeof(*alpine_inits)));
    case DISTRO_GENTOO:
        return find_exec(gentoo_inits, (int)(sizeof(gentoo_inits)/sizeof(*gentoo_inits)));
    case DISTRO_CUSTOM:
        return find_exec(custom_inits, (int)(sizeof(custom_inits)/sizeof(*custom_inits)));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* --- parse argument --- */
    if (argc < 2 || argc > 3) {
        fprintf(stderr,
                "Usage: %s --archlinux|--fedora|--ubuntu|--alpine|--gentoo|--rhel|--custom <path|dev|img>\n", argv[0]);
        execl("/bin/sh", "/bin/sh", NULL);
        return 1;
    }

    Distro distro;
    if      (strcmp(argv[1], "--archlinux") == 0) distro = DISTRO_ARCHLINUX;
    else if (strcmp(argv[1], "--fedora")    == 0) distro = DISTRO_FEDORA;
    else if (strcmp(argv[1], "--ubuntu")    == 0) distro = DISTRO_UBUNTU;
    else if (strcmp(argv[1], "--alpine")    == 0) distro = DISTRO_ALPINE;
    else if (strcmp(argv[1], "--gentoo")    == 0) distro = DISTRO_GENTOO;
    else if (strcmp(argv[1], "--rhel")      == 0) distro = DISTRO_RHEL;
    else if (strncmp(argv[1], "--custom=", 9) == 0) {
        distro = DISTRO_CUSTOM;
        snprintf(custom_path_global, sizeof(custom_path_global), "%s", argv[1] + 9);
    }
    else if (strcmp(argv[1], "--custom") == 0 && argc == 3) {
        distro = DISTRO_CUSTOM;
        snprintf(custom_path_global, sizeof(custom_path_global), "%s", argv[2]);
    }
    else {
        fprintf(stderr,
                "Usage: %s --archlinux|--fedora|--ubuntu|--alpine|--gentoo|--rhel|--custom <path|dev|img>\n", argv[0]);
        execl("/bin/sh", "/bin/sh", NULL);
        return 1;
    }

    const char *target = distro_target(distro);
    LOG("distro=%s target=%s", distro_name(distro), target);

    /* --- validate target (dir, block device, or image file) --- */
    struct stat st;
    if (stat(target, &st) != 0) {
        ERR("missing target: %s", target);
        execl("/bin/sh", "/bin/sh", NULL);
        return 1;
    }

    /* --- Device & Image Handling --- */
    if (S_ISBLK(st.st_mode)) {
        LOG("target is a block device, attempting to mount...");
        const char *mnt_point = "/mnt/custom_root";
        
        if (mount_block_device(target, mnt_point) != 0) {
            execl("/bin/sh", "/bin/sh", NULL);
            return 1;
        }
        target = mnt_point;
    } 
    else if (S_ISREG(st.st_mode)) {
        LOG("target is a regular file (image/squashfs), attempting loop mount...");
        const char *mnt_point = "/mnt/custom_root";
        
        if (mount_file_image(target, mnt_point) != 0) {
            execl("/bin/sh", "/bin/sh", NULL);
            return 1;
        }
        target = mnt_point;
    } 
    else if (!S_ISDIR(st.st_mode)) {
        ERR("target %s is neither a directory, block device, nor a regular file", target);
        execl("/bin/sh", "/bin/sh", NULL);
        return 1;
    }

    /* --- run phases --- */
    mount_host_api(distro);
    prepare_target_mounts(target, distro);
    do_pivot(target, distro);
    cleanup_old_root();
    post_pivot_fixups(distro);

    /* --- find and exec init --- */
    const char *init = find_init(distro);
    if (!init) {
        ERR("no init found for distro %s", distro_name(distro));
        execl("/bin/sh", "/bin/sh", NULL);
        return 1;
    }

    LOG("exec %s", init);
    execl(init, init, NULL);

    /* execl only returns on failure */
    ERR("exec %s: %s", init, strerror(errno));
    execl("/bin/sh", "/bin/sh", NULL);
    return 1;
}
