#include <mcp/mcp_forth.h>

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/boardctl.h>
#include <sys/mount.h>

#ifdef CONFIG_MCP_APPS_MCP_FS
    #include <mcp/mcp_fs.h>
#endif

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#define HAVE_NATIVE
#if defined(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_X86_32)
    #define NATIVE_ARCH     M4_ARCH_X86_32
    #define NATIVE_BACKEND  m4_x86_32_backend
    #define NATIVE_RUN_FUNC m4_x86_32_engine_run
#elif defined(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_ESP32S3)
    #define NATIVE_ARCH     M4_ARCH_ESP32S3
    #define NATIVE_BACKEND  m4_esp32s3_backend
    #define NATIVE_RUN_FUNC m4_esp32s3_engine_run
#else
    #undef HAVE_NATIVE
#endif

#define MEMORY_SIZE 2048

#ifdef HAVE_NATIVE
    #define DL_PATH "/tmp/"
    #define DL_PREFIX "m4_dl_"
    #define DL_PATH_MAX (sizeof(DL_PATH DL_PREFIX) + 10)
    #define DL_CTR_NAME "m4_dl_ctr"
#endif

static void show_usage(void)
{
    fprintf(stderr, "usage: mcp_forth [-m] [-O] <file path>\n");
}

int main(int argc, char *argv[])
{
    int res;
    ssize_t rwres;

    int fd;
    struct stat st;

    char * path;
    bool mount_samples = false;
    bool native = false;
    int opt;
    while((opt = getopt(argc, argv, "mO")) >= 0) {
        if(opt == 'm') mount_samples = true;
        else if(opt == 'O') native = true;
        else {
            show_usage();
            return 1;
        }
    }
    (void)mount_samples; /* suppress unused warnings */
    (void)native;

#ifdef CONFIG_MCP_APPS_MCP_FORTH_SAMPLES_ROMFS
    if(mount_samples) {
        res = stat("/forth_programs", &st);
        if(res < 0) {
            assert(errno == ENOENT);
            extern const unsigned char m4_samples_romfs_img[];
            extern unsigned int m4_samples_romfs_img_len;
            assert(m4_samples_romfs_img_len % 512 == 0);
            struct boardioc_romdisk_s romdisk_dsc = {
                .minor = CONFIG_MCP_APPS_MCP_FORTH_SAMPLES_ROMFS_MINOR_NO,
                .nsectors = m4_samples_romfs_img_len / 512,
                .sectsize = 512,
                .image = (uint8_t *)m4_samples_romfs_img
            };
            res = boardctl(BOARDIOC_ROMDISK, (uintptr_t) &romdisk_dsc);
            assert(res == 0);
            res = mount("/dev/ram" STRINGIFY(CONFIG_MCP_APPS_MCP_FORTH_SAMPLES_ROMFS_MINOR_NO),
                        "/forth_programs", "romfs", MS_RDONLY, NULL);
            assert(res == 0);
        }
    }
#endif

    if(optind >= argc) {
        if(!mount_samples) {
            show_usage();
            return 1;
        }
        return 0;
    }
    path = argv[optind];

#ifdef CONFIG_MCP_APPS_MCP_FS
    char * cachepath = mcp_fs_cache_file(path);
    if(cachepath) {
        path = cachepath;
    }
#endif

    fd = open(path, O_RDONLY);

#ifdef CONFIG_MCP_APPS_MCP_FS
    free(cachepath);
#endif

    if(fd == -1) {
        perror("open");
        return 1;
    }

    res = fstat(fd, &st);
    assert(res == 0);
    ssize_t buf_len = st.st_size;
    assert(buf_len >= 0);

    char * buf = malloc(buf_len);
    assert(buf);
    rwres = read(fd, buf, buf_len);
    assert(rwres == buf_len);

    res = close(fd);
    assert(res == 0);

    const m4_backend_t * backend = &m4_compact_bytecode_vm_backend;
#ifdef HAVE_NATIVE
    if(native) {
        backend = &NATIVE_BACKEND;
    }
#endif

    uint8_t * bin;
    int code_offset;
    int error_near;
    int bin_len = m4_compile(buf, buf_len, &bin, &code_offset,
        backend, &error_near);
    free(buf);
    if(bin_len < 0) {
        fprintf(stderr, "m4_compile: error %d near %d\n", bin_len, error_near);
        return 1;
    }
    uint8_t * code = NULL;

    m4_engine_run_t run_func = m4_vm_engine_run;
#ifdef HAVE_NATIVE
    char dl_path[DL_PATH_MAX];
    void * dl_handle;
    if(native) {
        run_func = NATIVE_RUN_FUNC;

        sem_t * sem = sem_open(DL_CTR_NAME, O_CREAT, 0666, 1);
        assert(sem != SEM_FAILED);
        res = sem_wait(sem);
        assert(res == 0);

        fd = open(DL_PATH DL_CTR_NAME, O_CREAT | O_RDWR, 0666);
        assert(fd >= 0);
        FILE * f = fdopen(fd, "r+");
        assert(f);
        unsigned ctr;
        res = fscanf(f, "%u", &ctr);
        if(res != 1) {
            assert(res == EOF);
            assert(!ferror(f));
            ctr = 0;
        }
        rewind(f);
        res = fprintf(f, "%u\n", ctr + 1);
        assert(res > 0);
        res = fclose(f);
        assert(res == 0);

        res = sem_post(sem);
        assert(res == 0);
        res = sem_close(sem);
        assert(res == 0);

        int elf_size = m4_elf_nuttx_size();
        void * elf = malloc(elf_size);
        assert(elf);
        m4_elf_nuttx(elf, NATIVE_ARCH, code_offset, bin_len - code_offset);

        snprintf(dl_path, sizeof(dl_path), DL_PATH DL_PREFIX "%u", ctr);

        fd = open(dl_path, O_CREAT | O_EXCL | O_WRONLY, 0666);
        assert(fd >= 0);

        rwres = write(fd, elf, elf_size);
        assert(rwres == elf_size);
        free(elf);

        rwres = write(fd, bin, bin_len);
        assert(rwres == bin_len);
        free(bin);

        res = close(fd);
        assert(res == 0);

        dl_handle = dlopen(dl_path, RTLD_NOW | RTLD_LOCAL);
        assert(dl_handle);

        bin = dlsym(dl_handle, "cont");
        assert(bin);
        code = dlsym(dl_handle, "code");
        assert(code);
    }
#endif

    static const m4_runtime_cb_array_t * cbs[] = {
        m4_runtime_lib_io,
        m4_runtime_lib_string,
        m4_runtime_lib_time,
        m4_runtime_lib_assert,
        M4_RUNTIME_LIB_MCP_ALL_ENTRIES
        NULL
    };

    uint8_t * memory = malloc(MEMORY_SIZE);
    assert(memory);
    const char * missing_word;
    int engine_res = run_func(
        bin,
        code,
        memory,
        MEMORY_SIZE,
        cbs,
        &missing_word
    );
    free(memory);

#ifdef HAVE_NATIVE
    if(native) {
        res = dlclose(dl_handle);
        assert(res == 0);
        res = unlink(dl_path);
        assert(res == 0);
    }
    else
#else
    {
        free(bin);
    }
#endif

    m4_global_cleanup();

    int ret = 0;
    if(engine_res == M4_RUNTIME_WORD_MISSING_ERROR) {
        fprintf(stderr, "runtime word \"%s\" missing\n", missing_word);
        ret = 1;
    }
    else if(engine_res) {
        fprintf(stderr, "engine error %d\n", engine_res);
        ret = 1;
    }

    return ret;
}
