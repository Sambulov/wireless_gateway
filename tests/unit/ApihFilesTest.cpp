/* Файловая служба (main/apih_files.c) на хосте: sys.files и
 * sys.file.write/delete/rename/mkdir/rmdir через настоящее ядро API, с
 * настоящими файлами. «Смонтированная ФС» — временный каталог (fs_store_stub),
 * работа с файлами — тот же POSIX, что идёт через VFS на приборе.
 *
 * Клиент — кадр в стаб-транспорт плюс такт ядра; ответ — последний кадр,
 * ушедший в транспорт. */
extern "C" {
#include "app.h"
#include "api.h"
#include "platform.h"
#include "apih_sys_internal.h"
#include "Converters/Base64Encoding.h"
}
#include "system_stub.h"
#include "transport_stub.h"
#include "net_stub.h"
#include "fs_store_stub.h"
#include "CppUTest/TestHarness.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
    void apih_files_test_reset(void);
}

/* Корень — массив, а не std::string: глобальная строка, переаллоцируемая
 * между тестами, — «утечка» для детектора CppUTest. */
static char root[64];

static std::string full(const char *p) { return std::string(root) + p; }

static bool exists(const char *p) {
    struct stat st;
    return stat(full(p).c_str(), &st) == 0;
}

static std::string read_file(const char *p) {
    std::string out;
    FILE *f = fopen(full(p).c_str(), "rb");
    if (!f) return "<нет файла>";
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static void write_file(const char *p, const char *data) {
    FILE *f = fopen(full(p).c_str(), "wb");
    fputs(data, f);
    fclose(f);
}

static void rm_tree(const std::string &dir) {
    DIR *d = opendir(dir.c_str());
    if (!d) { unlink(dir.c_str()); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        rm_tree(dir + "/" + e->d_name);
    }
    closedir(d);
    rmdir(dir.c_str());
}

static std::string b64(const std::string &raw) {
    std::string out(base64_encode_buffer_required((uint32_t)raw.size()) + 1, '\0');
    int32_t n = base64_encode((uint8_t *)&out[0], (uint32_t)out.size(),
                              (const uint8_t *)raw.data(), (uint32_t)raw.size());
    out.resize(n > 0 ? (size_t)n : 0);
    return out;
}

TEST_GROUP(ApihFiles) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    int sid;

    void setup() {
        char tmpl[] = "/tmp/wg_files_XXXXXX";
        CHECK(mkdtemp(tmpl) != NULL);
        snprintf(root, sizeof(root), "%s", tmpl);
        fs_store_stub_set_root(root);
        fs_store_stub_set_usage(2 * 1024 * 1024, 1000);

        memset(&app, 0, sizeof(app));
        system_stub_reset();
        ws_transport_stub_reset();
        net_stub_reset();
        api  = ws_transport_stub_init();
        conn = ws_transport_stub_connect(1);
        app.api_ap  = api;
        app.api_sta = NULL;
        apih_sys_common_init(&app);
        apih_files_test_reset();
        LONGS_EQUAL(ESP_OK, apih_files_run(&app));
        sid = 1;
    }
    void teardown() {
        apih_files_test_reset();
        ws_transport_stub_reset();
        api_deinit(api);
        rm_tree(root);
    }

    const char *call(const char *fid, const std::string &arg) {
        std::string frame = "{\"FID\":\"" + std::string(fid) + "\",\"SID\":" +
                            std::to_string(sid++) + ",\"ARG\":" + arg + "}";
        ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, frame.c_str(), frame.size(), 1);
        api_worker(api);
        return ws_transport_stub_last_sent(conn);
    }

    const char *put(const char *path, int offset, const std::string &raw, int last) {
        return call(ESP_WS_API_SYS_FILE_WRITE,
                    "{\"PATH\":\"" + std::string(path) + "\",\"OFFSET\":" + std::to_string(offset) +
                    ",\"DATA\":\"" + b64(raw) + "\",\"LAST\":" + std::to_string(last) + "}");
    }
};

/* ── листинг ──────────────────────────────────────────────────────────────── */

TEST(ApihFiles, ListingShowsFilesAndDirectoriesButHidesDotFiles) {
    write_file("/a.txt", "12345");
    mkdir(full("/maps").c_str(), 0755);
    write_file("/.hidden.part", "x");

    const char *r = call(ESP_WS_API_SYS_FILES, "{\"PATH\":\"/\"}");
    STRCMP_CONTAINS("\"STA\":\"0x00000000\"", r);
    STRCMP_CONTAINS("\"TOTAL\":2097152", r);
    STRCMP_CONTAINS("\"USED\":1000", r);
    STRCMP_CONTAINS("\"NAME\":\"a.txt\"", r);
    STRCMP_CONTAINS("\"SIZE\":5", r);
    STRCMP_CONTAINS("\"NAME\":\"maps\"", r);
    CHECK(strstr(r, ".hidden.part") == NULL);
}

TEST(ApihFiles, ListingRefusesPathsEscapingTheRoot) {
    const char *r = call(ESP_WS_API_SYS_FILES, "{\"PATH\":\"/../etc\"}");
    STRCMP_CONTAINS("\"ERR\":\"PATH\"", r);
}

/* ── запись порциями ─────────────────────────────────────────────────────── */

TEST(ApihFiles, FileIsWrittenInChunksAndAppearsOnlyOnLast) {
    STRCMP_CONTAINS("\"WRITTEN\":6", put("/doc.txt", 0, "hello ", 0));
    CHECK_FALSE(exists("/doc.txt"));                 /* пишется во временный */
    STRCMP_CONTAINS("\"WRITTEN\":11", put("/doc.txt", 6, "world", 1));
    STRCMP_EQUAL("hello world", read_file("/doc.txt").c_str());
    CHECK_FALSE(exists("/.doc.txt.part"));           /* временный убран */
}

TEST(ApihFiles, OverwriteReplacesTheFileOnlyWhenComplete) {
    write_file("/cfg.json", "old");
    put("/cfg.json", 0, "new-", 0);
    STRCMP_EQUAL("old", read_file("/cfg.json").c_str());   /* до LAST не тронут */
    put("/cfg.json", 4, "content", 1);
    STRCMP_EQUAL("new-content", read_file("/cfg.json").c_str());
}

TEST(ApihFiles, ChunkWithWrongOffsetIsRefused) {
    put("/doc.txt", 0, "abc", 0);
    STRCMP_CONTAINS("\"ERR\":\"OFFSET\"", put("/doc.txt", 10, "def", 1));
}

TEST(ApihFiles, ChunkWithoutAStartedUploadIsRefused) {
    STRCMP_CONTAINS("\"ERR\":\"OFFSET\"", put("/doc.txt", 3, "def", 1));
}

TEST(ApihFiles, BrokenBase64IsRefusedAndLeavesNoTempFile) {
    const char *r = call(ESP_WS_API_SYS_FILE_WRITE,
                         "{\"PATH\":\"/x.bin\",\"OFFSET\":0,\"DATA\":\"QU=I\",\"LAST\":1}");
    STRCMP_CONTAINS("\"ERR\":\"DATA\"", r);
    CHECK_FALSE(exists("/x.bin"));
    CHECK_FALSE(exists("/.x.bin.part"));
}

TEST(ApihFiles, WriteIntoADirectoryPathIsRefused) {
    mkdir(full("/maps").c_str(), 0755);
    STRCMP_CONTAINS("\"ERR\":\"PATH\"", put("/maps", 0, "x", 1));
}

TEST(ApihFiles, WritePathEscapingTheRootIsRefused) {
    STRCMP_CONTAINS("\"ERR\":\"PATH\"", put("/../evil.txt", 0, "x", 1));
    CHECK_FALSE(exists("/../evil.txt"));
}

/* ── удаление, переименование, каталоги ─────────────────────────────────── */

TEST(ApihFiles, DeleteRemovesTheFile) {
    write_file("/a.txt", "x");
    STRCMP_CONTAINS("\"STA\":\"0x00000000\"", call(ESP_WS_API_SYS_FILE_DELETE, "{\"PATH\":\"/a.txt\"}"));
    CHECK_FALSE(exists("/a.txt"));
}

TEST(ApihFiles, DeleteOfAMissingFileIsRefused) {
    STRCMP_CONTAINS("\"ERR\":\"PATH\"", call(ESP_WS_API_SYS_FILE_DELETE, "{\"PATH\":\"/none\"}"));
}

TEST(ApihFiles, RenameMovesTheFileAndRefusesToOverwrite) {
    write_file("/a.txt", "A");
    write_file("/b.txt", "B");
    STRCMP_CONTAINS("\"ERR\":\"EXIST\"",
                    call(ESP_WS_API_SYS_FILE_RENAME, "{\"FROM\":\"/a.txt\",\"TO\":\"/b.txt\"}"));
    STRCMP_EQUAL("B", read_file("/b.txt").c_str());

    STRCMP_CONTAINS("\"STA\":\"0x00000000\"",
                    call(ESP_WS_API_SYS_FILE_RENAME, "{\"FROM\":\"/a.txt\",\"TO\":\"/c.txt\"}"));
    CHECK_FALSE(exists("/a.txt"));
    STRCMP_EQUAL("A", read_file("/c.txt").c_str());
}

TEST(ApihFiles, MkdirAndRmdir) {
    STRCMP_CONTAINS("\"STA\":\"0x00000000\"", call(ESP_WS_API_SYS_FILE_MKDIR, "{\"PATH\":\"/maps\"}"));
    CHECK(exists("/maps"));
    STRCMP_CONTAINS("\"ERR\":\"EXIST\"", call(ESP_WS_API_SYS_FILE_MKDIR, "{\"PATH\":\"/maps\"}"));
    STRCMP_CONTAINS("\"STA\":\"0x00000000\"", call(ESP_WS_API_SYS_FILE_RMDIR, "{\"PATH\":\"/maps\"}"));
    CHECK_FALSE(exists("/maps"));
}

TEST(ApihFiles, RmdirOfANonEmptyDirectoryIsRefused) {
    mkdir(full("/maps").c_str(), 0755);
    write_file("/maps/x.jsonc", "{}");
    const char *r = call(ESP_WS_API_SYS_FILE_RMDIR, "{\"PATH\":\"/maps\"}");
    CHECK(strstr(r, "\"STA\":\"0x00000000\"") == NULL);
    CHECK(exists("/maps/x.jsonc"));
}
