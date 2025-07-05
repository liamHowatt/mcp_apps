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

#ifdef CONFIG_MCP_APPS_MCP_FS
    #include <mcp/mcp_fs.h>
#endif

#define HAVE_NATIVE
#if defined(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_X86_32)
    #define NATIVE_ARCH     M4_ARCH_X86_32
    #define NATIVE_BACKEND  m4_x86_32_backend
    #define NATIVE_RUN_FUNC m4_x86_32_engine_run
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
    fprintf(stderr, "usage: mcp_forth [-O] <file path>\n");
}

static bool is_native_flag(const char * arg)
{
    return arg[0] == '-' && arg[1] == 'O';
}

int main(int argc, char *argv[])
{
    int res;
    ssize_t rwres;

    int fd;

    char * path;
    bool native = false;
    if(argc == 2) {
        if(is_native_flag(argv[1])) {
            show_usage();
            return 1;
        }
        path = argv[1];
    }
    else if(argc == 3) {
        native = true;
        if(is_native_flag(argv[1])) {
            path = argv[2];
        }
        else if(is_native_flag(argv[2])) {
            path = argv[1];
        }
        else {
            show_usage();
            return 1;
        }
    }
    else {
        show_usage();
        return 1;
    }
    (void)native; /* suppress unused warning */

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

    struct stat st;
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
    int error_near;
    int bin_len = m4_compile(buf, buf_len, &bin,
        backend, &error_near);
    free(buf);
    if(bin_len < 0) {
        fprintf(stderr, "m4_compile: error %d near %d\n", bin_len, error_near);
        return 1;
    }

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
        m4_elf_nuttx(elf, NATIVE_ARCH, bin_len);

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

        m4_elf_content_t * elf_cont = dlsym(dl_handle, "cont");
        assert(elf_cont);
        bin = elf_cont->bin;
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
        bin_len,
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
