#include "bindings.h"
#include <sys/mount.h>

static int umount_wrapper(const char * target)
{
    return umount2(target, 0);
}

const m4_runtime_cb_array_t m4_runtime_lib_mount[] = {
    {"mount", {m4_f15, mount}},
    {"umount", {m4_f11, umount_wrapper}}, /* umount is a macro in nuttx */
    {"umount2", {m4_f12, umount2}},

    {"blksszget", {m4_lit, (void *) (BLKSSZGET)}},
    {"blkgetsize", {m4_lit, (void *) (BLKGETSIZE)}},

    {"ms_rdonly", {m4_lit, (void *) (MS_RDONLY)}},
    {"ms_nosuid", {m4_lit, (void *) (MS_NOSUID)}},
    {"ms_nodev", {m4_lit, (void *) (MS_NODEV)}},
    {"ms_noexec", {m4_lit, (void *) (MS_NOEXEC)}},
    {"ms_synchronous", {m4_lit, (void *) (MS_SYNCHRONOUS)}},
    {"ms_remount", {m4_lit, (void *) (MS_REMOUNT)}},
    {"ms_mandlock", {m4_lit, (void *) (MS_MANDLOCK)}},
    {"ms_dirsync", {m4_lit, (void *) (MS_DIRSYNC)}},
    {"ms_nosymfollow", {m4_lit, (void *) (MS_NOSYMFOLLOW)}},
    {"ms_noatime", {m4_lit, (void *) (MS_NOATIME)}},

    {"mnt_force", {m4_lit, (void *) (MNT_FORCE)}},
    {"mnt_detach", {m4_lit, (void *) (MNT_DETACH)}},
    {"mnt_expire", {m4_lit, (void *) (MNT_EXPIRE)}},
    {"umount_nofollow", {m4_lit, (void *) (UMOUNT_NOFOLLOW)}},

    {NULL}
};
