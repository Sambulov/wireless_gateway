/*
 * Прогонщик для тех случаев, когда libFuzzer недоступен (нет clang).
 *
 * Собирается вместе с харнессом и вызывает его точки входа ровно так, как это
 * делал бы libFuzzer: сначала LLVMFuzzerInitialize, затем LLVMFuzzerTestOneInput
 * по одному входу. Это не мутирующий фаззинг, зато проверка корпуса (seeds) и
 * любой внешней нарезки входов работает под ASan/UBSan без clang:
 *
 *   tests/fuzz/build/ws_server_runner tests/fuzz/seeds tests/fuzz/seeds_seq
 *
 * Каталоги читаются рекурсивно, аргументы-файлы — как есть.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len);

static int files_run;
static int files_failed;

static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "не открыть %s\n", path);
        files_failed++;
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);

    LLVMFuzzerTestOneInput(buf, got);
    free(buf);
    files_run++;
}

static void run_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "нет такого пути: %s\n", path);
        files_failed++;
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        run_file(path);
        return;
    }

    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        run_path(child);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    /* libFuzzer передаёт хвост аргументов харнессу — здесь так же. */
    int init_argc = 1;
    char *init_argv0 = argv[0];
    char **init_argv = &init_argv0;
    if (LLVMFuzzerInitialize(&init_argc, &init_argv) != 0) {
        fprintf(stderr, "LLVMFuzzerInitialize не отработал\n");
        return 2;
    }

    if (argc < 2) {
        fprintf(stderr, "укажи файлы или каталоги с входами\n");
        return 2;
    }

    for (int i = 1; i < argc; i++)
        run_path(argv[i]);

    printf("прогнано входов: %d, ошибок открытия: %d\n", files_run, files_failed);
    return files_failed ? 1 : 0;
}
