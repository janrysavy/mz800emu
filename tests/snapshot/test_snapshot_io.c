/*
 * test_snapshot_io.c — unit testy pro snapshot ZIP I/O operace
 *
 * Testuje: snapshot_io_open_write, snapshot_io_open_read,
 *          snapshot_io_write_bin, snapshot_io_read_bin,
 *          snapshot_io_write_xml, snapshot_io_read_xml,
 *          snapshot_io_entry_exists, snapshot_io_compute_checksum,
 *          snapshot_io_open_hash, snapshot_io_close_hash
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <stdlib.h>

#include "emulator/snapshot/snapshot.h"
#include "emulator/snapshot/snapshot_io.h"

static const char *TMP_FILE = "tests/data/tmp/test_io.mzs";

void setUp(void) { }
void tearDown(void)
{
    remove(TMP_FILE);
}

/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* vytvoření a zavření prázdného snapshotu */
void test_io_create_empty(void)
{
    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL_MESSAGE(io, "snapshot_io_open_write vrátil NULL");
    snapshot_io_close(io);
}

/* otevření neexistujícího souboru → NULL */
void test_io_open_nonexistent(void)
{
    snapshot_io_t *io = snapshot_io_open_read("tests/data/tmp/neexistuje.mzs");
    TEST_ASSERT_NULL_MESSAGE(io, "Otevření neexistujícího souboru mělo vrátit NULL");
}

/* zápis a čtení malého binárního bloku */
void test_io_write_read_small_binary(void)
{
    uint8_t write_data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };
    uint8_t *read_data = NULL;
    size_t read_size = 0;

    /* zápis */
    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);

    en_SNAPSHOT_RESULT res = snapshot_io_write_bin(io, "test/data.bin", write_data, sizeof(write_data));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, res);
    snapshot_io_close(io);

    /* čtení */
    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);

    res = snapshot_io_read_bin(io, "test/data.bin", &read_data, &read_size);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, res);
    TEST_ASSERT_EQUAL_size_t(sizeof(write_data), read_size);
    TEST_ASSERT_EQUAL_MEMORY(write_data, read_data, sizeof(write_data));

    g_free(read_data);
    snapshot_io_close(io);
}

/* ================================================================
 * UNIT TESTY
 * ================================================================ */

/* zápis a čtení 64KB binárního bloku (celá RAM) */
void test_io_write_read_64k_binary(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const size_t SIZE = 65536;
    uint8_t *write_buf = g_malloc(SIZE);
    uint8_t *read_buf = NULL;
    size_t read_size = 0;

    /* naplnit pseudonáhodným vzorem */
    for (size_t i = 0; i < SIZE; i++) {
        write_buf[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }

    /* zápis */
    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_bin(io, "memory/ram.bin", write_buf, SIZE));
    snapshot_io_close(io);

    /* čtení */
    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_bin(io, "memory/ram.bin", &read_buf, &read_size));
    TEST_ASSERT_EQUAL_size_t(SIZE, read_size);
    TEST_ASSERT_EQUAL_MEMORY(write_buf, read_buf, SIZE);

    g_free(read_buf);
    g_free(write_buf);
    snapshot_io_close(io);
}

/* čtení binárních dat do pevného bufferu */
void test_io_read_bin_into(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t write_data[256];
    uint8_t read_buf[256];

    for (int i = 0; i < 256; i++) {
        write_data[i] = (uint8_t)i;
    }

    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_bin(io, "test/block.bin", write_data, 256));
    snapshot_io_close(io);

    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);

    memset(read_buf, 0, sizeof(read_buf));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_bin_into(io, "test/block.bin", read_buf, 256));
    TEST_ASSERT_EQUAL_MEMORY(write_data, read_buf, 256);

    snapshot_io_close(io);
}

/* čtení neexistující entry → chyba */
void test_io_read_nonexistent_entry(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* vytvořit prázdný archiv */
    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);
    snapshot_io_close(io);

    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);

    uint8_t *data = NULL;
    size_t size = 0;
    en_SNAPSHOT_RESULT res = snapshot_io_read_bin(io, "neexistuje.bin", &data, &size);
    TEST_ASSERT_NOT_EQUAL(SNAPSHOT_OK, res);
    TEST_ASSERT_NULL(data);

    snapshot_io_close(io);
}

/* zápis a čtení XML stringu */
void test_io_write_read_xml(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *xml = "<root><value>42</value></root>";
    char *read_xml = NULL;

    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_xml(io, "test/config.xml", xml));
    snapshot_io_close(io);

    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_xml(io, "test/config.xml", &read_xml));
    TEST_ASSERT_NOT_NULL(read_xml);
    TEST_ASSERT_EQUAL_STRING(xml, read_xml);

    g_free(read_xml);
    snapshot_io_close(io);
}

/* entry_exists */
void test_io_entry_exists(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t data[] = { 0x01 };

    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);
    snapshot_io_write_bin(io, "existuje.bin", data, 1);
    snapshot_io_close(io);

    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_TRUE(snapshot_io_entry_exists(io, "existuje.bin"));
    TEST_ASSERT_FALSE(snapshot_io_entry_exists(io, "neexistuje.bin"));
    snapshot_io_close(io);
}

/* vícenásobný zápis do jednoho archivu */
void test_io_multiple_entries(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t data_a[] = { 0xAA, 0xBB };
    uint8_t data_b[] = { 0xCC, 0xDD, 0xEE };
    const char *xml = "<test/>";

    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_bin(io, "a.bin", data_a, 2));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_bin(io, "b.bin", data_b, 3));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_xml(io, "c.xml", xml));
    snapshot_io_close(io);

    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);

    uint8_t *ra = NULL, *rb = NULL;
    char *rc = NULL;
    size_t sa, sb;

    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_bin(io, "a.bin", &ra, &sa));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_bin(io, "b.bin", &rb, &sb));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_xml(io, "c.xml", &rc));

    TEST_ASSERT_EQUAL_size_t(2, sa);
    TEST_ASSERT_EQUAL_size_t(3, sb);
    TEST_ASSERT_EQUAL_MEMORY(data_a, ra, 2);
    TEST_ASSERT_EQUAL_MEMORY(data_b, rb, 3);
    TEST_ASSERT_EQUAL_STRING(xml, rc);

    g_free(ra);
    g_free(rb);
    g_free(rc);
    snapshot_io_close(io);
}

/* checksum — dva archivy se stejným obsahem mají stejný checksum */
void test_io_checksum_consistency(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *tmp2 = "tests/data/tmp/test_io_2.mzs";
    uint8_t data[] = { 0x01, 0x02, 0x03 };

    /* vytvořit dva identické archivy */
    for (int i = 0; i < 2; i++) {
        const char *path = (i == 0) ? TMP_FILE : tmp2;
        snapshot_io_t *io = snapshot_io_open_write(path, 6);
        snapshot_io_write_bin(io, "data.bin", data, sizeof(data));
        snapshot_io_close(io);
    }

    /* porovnat checksums */
    snapshot_io_t *io1 = snapshot_io_open_read(TMP_FILE);
    snapshot_io_t *io2 = snapshot_io_open_read(tmp2);

    char *cs1 = snapshot_io_compute_checksum(io1);
    char *cs2 = snapshot_io_compute_checksum(io2);

    TEST_ASSERT_NOT_NULL(cs1);
    TEST_ASSERT_NOT_NULL(cs2);
    TEST_ASSERT_EQUAL_STRING(cs1, cs2);

    /* checksum má 64 hex znaků (SHA-256) */
    TEST_ASSERT_EQUAL_size_t(64, strlen(cs1));

    g_free(cs1);
    g_free(cs2);
    snapshot_io_close(io1);
    snapshot_io_close(io2);
    remove(tmp2);
}

/* různý obsah → různý checksum */
void test_io_checksum_differs_for_different_data(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *tmp2 = "tests/data/tmp/test_io_3.mzs";
    uint8_t data_a[] = { 0x01 };
    uint8_t data_b[] = { 0x02 };

    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 6);
    snapshot_io_write_bin(io, "data.bin", data_a, 1);
    snapshot_io_close(io);

    io = snapshot_io_open_write(tmp2, 6);
    snapshot_io_write_bin(io, "data.bin", data_b, 1);
    snapshot_io_close(io);

    snapshot_io_t *io1 = snapshot_io_open_read(TMP_FILE);
    snapshot_io_t *io2 = snapshot_io_open_read(tmp2);

    char *cs1 = snapshot_io_compute_checksum(io1);
    char *cs2 = snapshot_io_compute_checksum(io2);

    TEST_ASSERT_NOT_NULL(cs1);
    TEST_ASSERT_NOT_NULL(cs2);
    /* musí se lišit */
    TEST_ASSERT_NOT_EQUAL(0, strcmp(cs1, cs2));

    g_free(cs1);
    g_free(cs2);
    snapshot_io_close(io1);
    snapshot_io_close(io2);
    remove(tmp2);
}

/* různé úrovně komprese */
void test_io_compression_levels(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t data[1024];
    memset(data, 0xAA, sizeof(data)); /* kompresibilní data */

    /* komprese 0 (none) */
    snapshot_io_t *io = snapshot_io_open_write(TMP_FILE, 0);
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_bin(io, "data.bin", data, sizeof(data)));
    snapshot_io_close(io);

    /* ověřit čtení */
    io = snapshot_io_open_read(TMP_FILE);
    TEST_ASSERT_NOT_NULL(io);
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_read_bin(io, "data.bin", &read_data, &read_size));
    TEST_ASSERT_EQUAL_MEMORY(data, read_data, sizeof(data));
    g_free(read_data);
    snapshot_io_close(io);
}

/* hashovací režim: stejné zápisy = stejný hash, nic se nezapisuje */
void test_io_hash_mode_deterministic(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t data[] = { 0x01, 0x02, 0x03 };
    char h1[65], h2[65];

    snapshot_io_t *io = snapshot_io_open_hash();
    TEST_ASSERT_NOT_NULL(io);
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_bin(io, "a.bin", data, sizeof(data)));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_write_xml(io, "b.xml", "<x/>"));
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_close_hash(io, h1, sizeof(h1)));

    io = snapshot_io_open_hash();
    snapshot_io_write_bin(io, "a.bin", data, sizeof(data));
    snapshot_io_write_xml(io, "b.xml", "<x/>");
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_OK, snapshot_io_close_hash(io, h2, sizeof(h2)));

    TEST_ASSERT_EQUAL_INT(64, (int)strlen(h1));
    TEST_ASSERT_EQUAL_STRING(h1, h2);
}

/* hashovací režim: jiná data, jiné jméno i jiné rozdělení = jiný hash */
void test_io_hash_mode_differs(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t ab[] = { 'a', 'b' };
    char base[65], other[65];

    snapshot_io_t *io = snapshot_io_open_hash();
    snapshot_io_write_bin(io, "x.bin", ab, 2);
    snapshot_io_close_hash(io, base, sizeof(base));

    /* jiný obsah */
    uint8_t ac[] = { 'a', 'c' };
    io = snapshot_io_open_hash();
    snapshot_io_write_bin(io, "x.bin", ac, 2);
    snapshot_io_close_hash(io, other, sizeof(other));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(base, other));

    /* jiné jméno entry */
    io = snapshot_io_open_hash();
    snapshot_io_write_bin(io, "y.bin", ab, 2);
    snapshot_io_close_hash(io, other, sizeof(other));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(base, other));

    /* stejné bajty rozdělené do dvou entries */
    io = snapshot_io_open_hash();
    snapshot_io_write_bin(io, "x.bin", ab, 1);
    snapshot_io_write_bin(io, "x.bin", ab + 1, 1);
    snapshot_io_close_hash(io, other, sizeof(other));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(base, other));
}

/* hashovací režim: čtení selže, malý výstup = chyba */
void test_io_hash_mode_rejects_read_and_small_buffer(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    snapshot_io_t *io = snapshot_io_open_hash();
    uint8_t *out = NULL;
    size_t out_size = 0;
    TEST_ASSERT_NOT_EQUAL(SNAPSHOT_OK, snapshot_io_read_bin(io, "a.bin", &out, &out_size));
    TEST_ASSERT_FALSE(snapshot_io_entry_exists(io, "a.bin"));

    char small[10];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_IO, snapshot_io_close_hash(io, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);

    /* handle z běžného zápisu nemá hashovací režim */
    snapshot_io_t *zip = snapshot_io_open_write(TMP_FILE, 6);
    char h[65];
    TEST_ASSERT_EQUAL_INT(SNAPSHOT_ERR_IO, snapshot_io_close_hash(zip, h, sizeof(h)));
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    /* smoke */
    RUN_TEST(test_io_create_empty);
    RUN_TEST(test_io_open_nonexistent);
    RUN_TEST(test_io_write_read_small_binary);

    /* unit */
    RUN_TEST(test_io_write_read_64k_binary);
    RUN_TEST(test_io_read_bin_into);
    RUN_TEST(test_io_read_nonexistent_entry);
    RUN_TEST(test_io_write_read_xml);
    RUN_TEST(test_io_entry_exists);
    RUN_TEST(test_io_multiple_entries);
    RUN_TEST(test_io_checksum_consistency);
    RUN_TEST(test_io_checksum_differs_for_different_data);
    RUN_TEST(test_io_compression_levels);
    RUN_TEST(test_io_hash_mode_deterministic);
    RUN_TEST(test_io_hash_mode_differs);
    RUN_TEST(test_io_hash_mode_rejects_read_and_small_buffer);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
