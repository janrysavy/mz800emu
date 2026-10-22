/**
 * @file snapshot_io.c
 * @brief I/O vrstva snapshot systému — implementace nad minizip-ng
 */

#include "snapshot_io.h"
#include "snapshot_xml.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <glib.h>
#include <stdio.h>
#include <string.h>


/* Interní struktura I/O handle */
struct snapshot_io_t {
    void *handle;           /* minizip-ng writer nebo reader */
    bool writing;           /* true = zápis, false = čtení */
    bool is_buffer;         /* true = backed by mem stream, false = file */
    void *mem_stream;       /* mem stream při buffer módu, jinak NULL */
    int compression_level;  /* kompresní úroveň (jen pro zápis) */
    /* Pro inkrementální výpočet checksumu při zápisu */
    GPtrArray *written_names;  /* seznam názvů zapsaných entries */
    GPtrArray *written_data;   /* seznam GBytes* s daty entries */
    /* Hashovací režim (snapshot_io_open_hash): entries se nezapisují do ZIP,
     * jen se počítá SHA-256 přes jméno, délku a data; handle je NULL. */
    GChecksum *hash;
};


/* ========================================================================= */
/*                            Pomocné funkce                                 */
/* ========================================================================= */

/**
 * Kontrola bezpečnosti názvu entry (ochrana proti path traversal)
 */
static bool io_validate_entry_name(const char *entry_name)
{
    if (!entry_name || entry_name[0] == '\0') return false;

    /* Žádné absolutní cesty */
    if (entry_name[0] == '/' || entry_name[0] == '\\') return false;

    /* Žádné ".." v cestě */
    if (strstr(entry_name, "..") != NULL) return false;

    /* Žádné zpětné lomítko (Windows cesty) */
    if (strchr(entry_name, '\\') != NULL) return false;

    return true;
}


/* ========================================================================= */
/*                           Otevření / zavření                              */
/* ========================================================================= */

snapshot_io_t *snapshot_io_open_write(const char *filepath, int compression_level)
{
    if (!filepath) return NULL;

    void *writer = mz_zip_writer_create();
    if (!writer) return NULL;

    /* Nastavení kompresní metody a úrovně */
    mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(writer, (int16_t)compression_level);

    /* Otevření souboru pro zápis (disk_size=0 = jednodílný archiv, append=0) */
    int32_t err = mz_zip_writer_open_file(writer, filepath, 0, 0);
    if (err != MZ_OK) {
        SNAP_ERR("io", "cannot open file for writing: %s (err=%d)", filepath, err);
        mz_zip_writer_delete(&writer);
        return NULL;
    }

    snapshot_io_t *io = g_new0(snapshot_io_t, 1);
    io->handle = writer;
    io->writing = true;
    io->compression_level = compression_level;
    io->written_names = g_ptr_array_new_with_free_func(g_free);
    io->written_data = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);

    return io;
}


snapshot_io_t *snapshot_io_open_read(const char *filepath)
{
    if (!filepath) return NULL;

    void *reader = mz_zip_reader_create();
    if (!reader) return NULL;

    int32_t err = mz_zip_reader_open_file(reader, filepath);
    if (err != MZ_OK) {
        SNAP_ERR("io", "cannot open file for reading: %s (err=%d)", filepath, err);
        mz_zip_reader_delete(&reader);
        return NULL;
    }

    snapshot_io_t *io = g_new0(snapshot_io_t, 1);
    io->handle = reader;
    io->writing = false;

    return io;
}


void snapshot_io_close(snapshot_io_t *io)
{
    if (!io) return;

    if (io->hash) {
        g_checksum_free(io->hash);
        g_free(io);
        return;
    }

    if (io->writing) {
        mz_zip_writer_close(io->handle);
        mz_zip_writer_delete(&io->handle);
        if (io->written_names) g_ptr_array_free(io->written_names, TRUE);
        if (io->written_data) g_ptr_array_free(io->written_data, TRUE);
    } else {
        mz_zip_reader_close(io->handle);
        mz_zip_reader_delete(&io->handle);
    }

    /* Buffer-backed handle drží navíc vlastní mem stream
     * (vlastní jak writer-side rostoucí buffer, tak reader-side
     * read-only view). V obou případech jej uvolníme zde. */
    if (io->mem_stream) {
        mz_stream_mem_close(io->mem_stream);
        mz_stream_mem_delete(&io->mem_stream);
    }

    g_free(io);
}


/* ========================================================================= */
/*                       Buffer-based open / close                            */
/* ========================================================================= */

snapshot_io_t *snapshot_io_open_write_buffer(int compression_level)
{
    /* 1. Vytvořit prázdný mem stream (rostoucí, bez prealokovaného bufferu) */
    void *stream = mz_stream_mem_create();
    if (!stream) return NULL;

    /* Implicitní grow_size minizip-ng je typicky 4 KB; pro snapshot
     * (desítky až stovky KB) volíme větší krok, aby se snížil počet
     * realokací. */
    mz_stream_mem_set_grow_size(stream, 64 * 1024);

    /* 2. Otevřít stream v režimu zápisu (CREATE) */
    int32_t err = mz_stream_mem_open(stream, NULL,
                                     MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_WRITE);
    if (err != MZ_OK) {
        SNAP_ERR("io", "mem stream open (write) failed (err=%d)", err);
        mz_stream_mem_delete(&stream);
        return NULL;
    }

    /* 3. Vytvořit ZIP writer nad streamem */
    void *writer = mz_zip_writer_create();
    if (!writer) {
        mz_stream_mem_close(stream);
        mz_stream_mem_delete(&stream);
        return NULL;
    }

    mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(writer, (int16_t)compression_level);

    err = mz_zip_writer_open(writer, stream, 0);
    if (err != MZ_OK) {
        SNAP_ERR("io", "zip writer open over mem stream failed (err=%d)", err);
        mz_zip_writer_delete(&writer);
        mz_stream_mem_close(stream);
        mz_stream_mem_delete(&stream);
        return NULL;
    }

    snapshot_io_t *io = g_new0(snapshot_io_t, 1);
    io->handle = writer;
    io->writing = true;
    io->is_buffer = true;
    io->mem_stream = stream;
    io->compression_level = compression_level;
    io->written_names = g_ptr_array_new_with_free_func(g_free);
    io->written_data = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);

    return io;
}


snapshot_io_t *snapshot_io_open_read_buffer(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return NULL;

    /* int32_t je interní typ minizip-ng pro velikost streamu */
    if (size > (size_t)INT32_MAX) {
        SNAP_ERR("io", "input buffer too large (%zu B)", size);
        return NULL;
    }

    /* 1. Vytvořit mem stream nad poskytnutou pamětí (read-only view) */
    void *stream = mz_stream_mem_create();
    if (!stream) return NULL;

    /* set_buffer NEkopíruje - vystaví read-only pohled na (data, size).
     * mz_stream_mem cast-uje void* dovnitř; const odlomíme pouze pro toto
     * API volání. Naše API kontrakt: read-only používání, žádný write. */
    mz_stream_mem_set_buffer(stream, (void *)data, (int32_t)size);

    int32_t err = mz_stream_mem_open(stream, NULL, MZ_OPEN_MODE_READ);
    if (err != MZ_OK) {
        SNAP_ERR("io", "mem stream open (read) failed (err=%d)", err);
        mz_stream_mem_delete(&stream);
        return NULL;
    }

    /* 2. Vytvořit ZIP reader nad streamem */
    void *reader = mz_zip_reader_create();
    if (!reader) {
        mz_stream_mem_close(stream);
        mz_stream_mem_delete(&stream);
        return NULL;
    }

    err = mz_zip_reader_open(reader, stream);
    if (err != MZ_OK) {
        /* Typicky znamená korupci dat nebo to není ZIP */
        SNAP_ERR("io", "zip reader open over mem stream failed (err=%d)", err);
        mz_zip_reader_delete(&reader);
        mz_stream_mem_close(stream);
        mz_stream_mem_delete(&stream);
        return NULL;
    }

    snapshot_io_t *io = g_new0(snapshot_io_t, 1);
    io->handle = reader;
    io->writing = false;
    io->is_buffer = true;
    io->mem_stream = stream;

    return io;
}


en_SNAPSHOT_RESULT snapshot_io_close_to_buffer(snapshot_io_t *io,
                                              uint8_t **out_data,
                                              size_t *out_size)
{
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;

    if (!io || !out_data || !out_size) {
        /* Pokud handle není NULL, přesto se musí korektně zavřít */
        if (io) snapshot_io_close(io);
        return SNAPSHOT_ERR_IO;
    }

    if (!io->writing || !io->is_buffer || !io->mem_stream) {
        SNAP_ERR("io", "close_to_buffer called on non-buffer-writer handle");
        snapshot_io_close(io);
        return SNAPSHOT_ERR_IO;
    }

    /* 1. Zavřít ZIP writer - central directory se zapíše do streamu */
    int32_t err = mz_zip_writer_close(io->handle);
    if (err != MZ_OK) {
        SNAP_ERR("io", "zip writer close failed (err=%d)", err);
        /* Stále uvolnit zbytek (po zápisu central dir chyba bývá vzácná) */
        snapshot_io_close(io);
        return SNAPSHOT_ERR_ZIP;
    }

    /* 2. Vytáhnout pointer a délku z mem streamu */
    const void *buf = NULL;
    int32_t len = 0;
    int32_t rc = mz_stream_mem_get_buffer(io->mem_stream, &buf);
    if (rc != MZ_OK || !buf) {
        SNAP_ERR("io", "mem stream get_buffer failed (rc=%d)", rc);
        snapshot_io_close(io);
        return SNAPSHOT_ERR_IO;
    }
    mz_stream_mem_get_buffer_length(io->mem_stream, &len);
    if (len <= 0) {
        SNAP_ERR("io", "mem stream produced empty buffer (len=%d)", len);
        snapshot_io_close(io);
        return SNAPSHOT_ERR_IO;
    }

    /* 3. Zkopírovat data do nově alokovaného bufferu, který přebírá volající.
     *    Konvence g_malloc/g_free sjednocena s ostatním snapshot API
     *    (viz snapshot_extract_screenshot). */
    uint8_t *copy = g_malloc((size_t)len);
    if (!copy) {
        snapshot_io_close(io);
        return SNAPSHOT_ERR_ALLOC;
    }
    memcpy(copy, buf, (size_t)len);

    /* 4. Uvolnit handle - writer i mem stream. mz_zip_writer_close uz
     *    proběhl, takže mz_zip_writer_delete jen uvolní strukturu;
     *    sentinel uvnitř snapshot_io_close vyhodnotí znovu close (no-op
     *    pro již zavřený writer u minizip-ng). */
    mz_zip_writer_delete(&io->handle);
    /* Zabraň double-close v snapshot_io_close - handle už je NULL po delete. */
    if (io->written_names) g_ptr_array_free(io->written_names, TRUE);
    if (io->written_data) g_ptr_array_free(io->written_data, TRUE);
    io->written_names = NULL;
    io->written_data = NULL;
    mz_stream_mem_close(io->mem_stream);
    mz_stream_mem_delete(&io->mem_stream);
    g_free(io);

    *out_data = copy;
    *out_size = (size_t)len;
    return SNAPSHOT_OK;
}


/* ========================================================================= */
/*                          Hashovací režim                                  */
/* ========================================================================= */

snapshot_io_t *snapshot_io_open_hash(void)
{
    snapshot_io_t *io = g_new0(snapshot_io_t, 1);
    io->writing = true;
    io->hash = g_checksum_new(G_CHECKSUM_SHA256);
    return io;
}


en_SNAPSHOT_RESULT snapshot_io_close_hash(snapshot_io_t *io, char *out_hex, size_t out_size)
{
    if (out_hex && out_size) out_hex[0] = '\0';
    if (!io || !io->hash) {
        if (io) snapshot_io_close(io);
        return SNAPSHOT_ERR_IO;
    }
    const char *hex = g_checksum_get_string(io->hash);
    en_SNAPSHOT_RESULT res = SNAPSHOT_OK;
    if (!out_hex || out_size <= strlen(hex)) res = SNAPSHOT_ERR_IO;
    else g_strlcpy(out_hex, hex, out_size);
    snapshot_io_close(io);
    return res;
}


/* ========================================================================= */
/*                              Zápis                                        */
/* ========================================================================= */

en_SNAPSHOT_RESULT snapshot_io_write_bin(snapshot_io_t *io,
                                        const char *entry_name,
                                        const uint8_t *data,
                                        size_t size)
{
    if (!io || !io->writing || !entry_name || !data) return SNAPSHOT_ERR_IO;
    if (!io_validate_entry_name(entry_name)) return SNAPSHOT_ERR_IO;

    if (io->hash) {
        /* Jméno s nulou a délka (8 bajtů LE) oddělují entries, takže stejná
         * data rozdělená jinak dají jiný hash. */
        guint8 len_le[8];
        guint64 len = (guint64)size;
        for (int i = 0; i < 8; i++) len_le[i] = (guint8)(len >> (8 * i));
        g_checksum_update(io->hash, (const guchar *)entry_name, (gssize)strlen(entry_name) + 1);
        g_checksum_update(io->hash, len_le, sizeof(len_le));
        if (size) g_checksum_update(io->hash, data, (gssize)size);
        return SNAPSHOT_OK;
    }

    /* Připravit file_info pro nový entry */
    mz_zip_file file_info;
    memset(&file_info, 0, sizeof(file_info));
    file_info.filename = entry_name;
    file_info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    file_info.modified_date = time(NULL);

    int32_t err = mz_zip_writer_add_buffer(io->handle, data, (int32_t)size, &file_info);
    if (err != MZ_OK) {
        SNAP_ERR("io", "write error '%s' (err=%d)", entry_name, err);
        return SNAPSHOT_ERR_ZIP;
    }

    /* Uložit kopii dat pro výpočet checksumu */
    if (io->written_names && strcmp(entry_name, "manifest.xml") != 0) {
        g_ptr_array_add(io->written_names, g_strdup(entry_name));
        g_ptr_array_add(io->written_data, g_bytes_new(data, size));
    }

    return SNAPSHOT_OK;
}


en_SNAPSHOT_RESULT snapshot_io_write_xml(snapshot_io_t *io,
                                        const char *entry_name,
                                        const char *xml_string)
{
    if (!io || !xml_string) return SNAPSHOT_ERR_IO;

    size_t len = strlen(xml_string);
    return snapshot_io_write_bin(io, entry_name, (const uint8_t *)xml_string, len);
}


/* ========================================================================= */
/*                              Čtení                                        */
/* ========================================================================= */

en_SNAPSHOT_RESULT snapshot_io_read_bin(snapshot_io_t *io,
                                       const char *entry_name,
                                       uint8_t **out_data,
                                       size_t *out_size)
{
    if (!io || io->writing || !entry_name || !out_data || !out_size)
        return SNAPSHOT_ERR_IO;

    /* Najít entry v archivu */
    int32_t err = mz_zip_reader_locate_entry(io->handle, entry_name, 0);
    if (err != MZ_OK) {
        return SNAPSHOT_ERR_MISSING_FILE;
    }

    /* Otevřít entry pro čtení */
    err = mz_zip_reader_entry_open(io->handle);
    if (err != MZ_OK) {
        SNAP_ERR("io", "cannot open entry '%s' (err=%d)", entry_name, err);
        return SNAPSHOT_ERR_ZIP;
    }

    /* Zjistit velikost */
    int32_t buf_len = mz_zip_reader_entry_save_buffer_length(io->handle);
    if (buf_len < 0) {
        mz_zip_reader_entry_close(io->handle);
        SNAP_ERR("io", "cannot get size of entry '%s'", entry_name);
        return SNAPSHOT_ERR_ZIP;
    }

    /* Kontrola velikosti */
    if ((size_t)buf_len > SNAPSHOT_MAX_ARCHIVE_SIZE) {
        mz_zip_reader_entry_close(io->handle);
        SNAP_ERR("io", "entry '%s' is too large (%d B)", entry_name, buf_len);
        return SNAPSHOT_ERR_CORRUPTED;
    }

    /* Alokovat buffer a přečíst data */
    uint8_t *buffer = g_malloc(buf_len);
    if (!buffer) {
        mz_zip_reader_entry_close(io->handle);
        return SNAPSHOT_ERR_ALLOC;
    }

    err = mz_zip_reader_entry_save_buffer(io->handle, buffer, buf_len);
    mz_zip_reader_entry_close(io->handle);

    if (err != MZ_OK) {
        g_free(buffer);
        SNAP_ERR("io", "read error in entry '%s' (err=%d)", entry_name, err);
        return SNAPSHOT_ERR_ZIP;
    }

    *out_data = buffer;
    *out_size = (size_t)buf_len;
    return SNAPSHOT_OK;
}


en_SNAPSHOT_RESULT snapshot_io_read_bin_into(snapshot_io_t *io,
                                            const char *entry_name,
                                            uint8_t *buffer,
                                            size_t buffer_size)
{
    uint8_t *data = NULL;
    size_t data_size = 0;

    en_SNAPSHOT_RESULT result = snapshot_io_read_bin(io, entry_name, &data, &data_size);
    if (result != SNAPSHOT_OK) return result;

    /* Ověřit velikost */
    if (data_size != buffer_size) {
        SNAP_ERR("io", "size of '%s' mismatch: expected %zu, got %zu",
                 entry_name, buffer_size, data_size);
        g_free(data);
        return SNAPSHOT_ERR_CORRUPTED;
    }

    memcpy(buffer, data, data_size);
    g_free(data);
    return SNAPSHOT_OK;
}


en_SNAPSHOT_RESULT snapshot_io_read_xml(snapshot_io_t *io,
                                       const char *entry_name,
                                       char **out_xml_string)
{
    uint8_t *data = NULL;
    size_t data_size = 0;

    en_SNAPSHOT_RESULT result = snapshot_io_read_bin(io, entry_name, &data, &data_size);
    if (result != SNAPSHOT_OK) return result;

    /* Přidat null terminator */
    char *str = g_malloc(data_size + 1);
    if (!str) {
        g_free(data);
        return SNAPSHOT_ERR_ALLOC;
    }

    memcpy(str, data, data_size);
    str[data_size] = '\0';
    g_free(data);

    *out_xml_string = str;
    return SNAPSHOT_OK;
}


bool snapshot_io_entry_exists(snapshot_io_t *io, const char *entry_name)
{
    if (!io || io->writing || !entry_name) return false;

    int32_t err = mz_zip_reader_locate_entry(io->handle, entry_name, 0);
    return (err == MZ_OK);
}


/* ========================================================================= */
/*                           SHA-256 checksum                                */
/* ========================================================================= */

/**
 * Porovnávací funkce pro qsort — abecední řazení názvů entries
 */
static int compare_entry_names(const void *a, const void *b)
{
    const char *name_a = *(const char **)a;
    const char *name_b = *(const char **)b;
    return strcmp(name_a, name_b);
}


/**
 * Výpočet checksumu ze zapsaných dat (writer mode)
 */
static char *compute_checksum_from_written_data(snapshot_io_t *io)
{
    if (!io->written_names || io->written_names->len == 0) return NULL;

    /* Vytvořit pole indexů seřazených dle jmen */
    guint count = io->written_names->len;
    guint *sorted_indices = g_new(guint, count);
    for (guint i = 0; i < count; i++) sorted_indices[i] = i;

    /* Seřadit indexy dle jmen */
    /* Bubble sort — jednoduchý pro malý počet entries */
    for (guint i = 0; i < count - 1; i++) {
        for (guint j = 0; j < count - i - 1; j++) {
            const char *a = g_ptr_array_index(io->written_names, sorted_indices[j]);
            const char *b = g_ptr_array_index(io->written_names, sorted_indices[j + 1]);
            if (strcmp(a, b) > 0) {
                guint tmp = sorted_indices[j];
                sorted_indices[j] = sorted_indices[j + 1];
                sorted_indices[j + 1] = tmp;
            }
        }
    }

    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    for (guint i = 0; i < count; i++) {
        GBytes *bytes = g_ptr_array_index(io->written_data, sorted_indices[i]);
        gsize size;
        const guint8 *data = g_bytes_get_data(bytes, &size);
        g_checksum_update(checksum, data, size);
    }

    const char *hex = g_checksum_get_string(checksum);
    char *result = g_strdup(hex);

    g_checksum_free(checksum);
    g_free(sorted_indices);

    return result;
}


/**
 * Výpočet checksumu z archivu (reader mode)
 */
static char *compute_checksum_from_archive(snapshot_io_t *io)
{
    /* Shromáždit názvy všech entries kromě manifest.xml */
    GPtrArray *entry_names = g_ptr_array_new_with_free_func(g_free);

    int32_t err = mz_zip_reader_goto_first_entry(io->handle);
    while (err == MZ_OK) {
        mz_zip_file *file_info = NULL;
        if (mz_zip_reader_entry_get_info(io->handle, &file_info) == MZ_OK && file_info->filename) {
            if (strcmp(file_info->filename, "manifest.xml") != 0) {
                g_ptr_array_add(entry_names, g_strdup(file_info->filename));
            }
        }
        err = mz_zip_reader_goto_next_entry(io->handle);
    }

    g_ptr_array_sort(entry_names, compare_entry_names);

    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (!checksum) {
        g_ptr_array_free(entry_names, TRUE);
        return NULL;
    }

    for (guint i = 0; i < entry_names->len; i++) {
        const char *name = g_ptr_array_index(entry_names, i);

        err = mz_zip_reader_locate_entry(io->handle, name, 0);
        if (err != MZ_OK) continue;

        err = mz_zip_reader_entry_open(io->handle);
        if (err != MZ_OK) continue;

        uint8_t buf[8192];
        int32_t bytes_read;
        while ((bytes_read = mz_zip_reader_entry_read(io->handle, buf, sizeof(buf))) > 0) {
            g_checksum_update(checksum, buf, bytes_read);
        }

        mz_zip_reader_entry_close(io->handle);
    }

    const char *hex = g_checksum_get_string(checksum);
    char *result = g_strdup(hex);

    g_checksum_free(checksum);
    g_ptr_array_free(entry_names, TRUE);

    return result;
}


char *snapshot_io_compute_checksum(snapshot_io_t *io)
{
    if (!io) return NULL;

    if (io->writing) {
        return compute_checksum_from_written_data(io);
    } else {
        return compute_checksum_from_archive(io);
    }
}
