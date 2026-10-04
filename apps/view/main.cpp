// view -- show a .gcx picture from the CF card.
//
//   VIEW FILE.GCX
//
// A .gcx file is a memory image (image-tools/gcx_target.h): the pixel plane,
// every line's VIDCMD words, and the ENGINE descriptor list that streams both,
// built and verified by the encoder.  So this app does no video work of its own.
// It reads the whole file into the heap, copies the descriptor table into the
// direct-video carve with each source turned from a file offset into an address,
// and then re-arms that table at every vsync until Enter is pressed.
//
// The list starts with the frame preamble and image-tools' WALK_LINES pacers, so
// it is armed the way every other list is: griffin_vsync_wait() returns inside
// the vsync line and the move.w to ENGINE_DESC follows straight away
// (firmware/rom.cpp VBLANK_WALK_LINES makes the same assumption).
//
// Enter, on the PS/2 keyboard or the serial console, ends the viewing.  The
// console stays in line mode rather than raw: raw scancodes come only from the
// PS/2 keyboard, and the line editor already sits above the make/break decoder,
// so the release of the Enter that started this app cannot end it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "../../image-tools/gcx_target.h"
#include "../../super-engine/descriptor.h"
#include "../lib/griffin_app.h"
#include "../lib/griffin_video.h"

using namespace Griffin::reg;

namespace
{

namespace se = SuperEngine;

// Read the whole file into one malloc.  The size comes from the file's own
// header (the firmware's SEEK_END and stat() cannot be trusted for it), and
// open_picture() then checks that the header and the bytes read agree.  One
// byte past the stated length is asked for, so a file longer than its header
// says is refused too.  Returns the buffer and its length in words, or nullptr.
uint16_t *load_file(const char *path, uint32_t *words)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        printf("view: cannot open %s\n", path);
        return nullptr;
    }

    uint16_t header[GcxTarget::HEADER_WORDS];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header))
    {
        printf("view: %s: shorter than a header\n", path);
        fclose(fp);
        return nullptr;
    }
    // The length field means nothing in a file that is not a .gcx at all, so
    // the magic is checked before the length is trusted for an allocation.
    if (GcxTarget::long_at(header, GcxTarget::HDR_MAGIC) != GcxTarget::MAGIC)
    {
        GcxTarget::Picture unused;
        const char        *why = nullptr;
        (void)GcxTarget::open_picture(header, GcxTarget::HEADER_WORDS, &unused, &why);
        printf("view: %s: %s\n", path, why);
        fclose(fp);
        return nullptr;
    }
    const uint32_t bytes = GcxTarget::long_at(header, GcxTarget::HDR_FILE_BYTES);
    if (bytes < sizeof(header) || (bytes & 1u) != 0)
    {
        printf("view: %s: bad file length %lu in header\n", path,
               static_cast<unsigned long>(bytes));
        fclose(fp);
        return nullptr;
    }

    // malloc, not new[]: a throwing array-new drags the libstdc++ exception
    // machinery into the binary for no gain here.  malloc's alignment is what
    // makes the load address even, which relocate_table() requires.
    uint16_t *file = static_cast<uint16_t *>(malloc(bytes + 1));
    if (!file)
    {
        printf("view: out of memory for %s (%lu bytes)\n", path,
               static_cast<unsigned long>(bytes));
        fclose(fp);
        return nullptr;
    }
    for (unsigned i = 0; i < GcxTarget::HEADER_WORDS; i++)
    {
        file[i] = header[i];
    }
    const uint32_t rest = bytes + 1 - sizeof(header);
    const size_t   got  = fread(reinterpret_cast<uint8_t *>(file) + sizeof(header), 1, rest, fp);
    fclose(fp);

    // Odd totals cannot pass open_picture()'s length check, so truncating to
    // words here is safe.
    *words = static_cast<uint32_t>((sizeof(header) + got) / 2);
    if (sizeof(header) + got > bytes)
    {
        printf("view: %s is longer than its header says (%lu bytes)\n", path,
               static_cast<unsigned long>(bytes));
        free(file);
        return nullptr;
    }
    return file;
}

// Drain the console until a line (or EOF) has been read.  griffin_inputready()
// pumps the line editor, so this is also what keeps the keyboard serviced.
bool enter_pressed()
{
    if (griffin_inputready() <= 0)
    {
        return false;
    }
    char line[64];
    (void)read(0, line, sizeof(line));
    return true;
}

}   // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: view FILE.GCX\n");
        return 1;
    }
    const char *path = argv[1];

    uint32_t  words = 0;
    uint16_t *file  = load_file(path, &words);
    if (!file)
    {
        return 1;
    }

    GcxTarget::Picture picture;
    const char        *why = nullptr;
    if (!GcxTarget::open_picture(file, words, &picture, &why))
    {
        printf("view: %s: %s\n", path, why);
        free(file);
        return 1;
    }

    GriffinVideoDirectInfo info;
    info.desc_table_base  = 0;
    info.desc_table_bytes = 0;
    if (griffin_video_direct_start(&info) != 0)
    {
        printf("view: cannot take direct control of the video\n");
        free(file);
        return 1;
    }
    if (picture.table_bytes() > info.desc_table_bytes)
    {
        (void)griffin_video_direct_end();
        printf("view: %s: %lu-byte descriptor table, the carve holds %lu\n", path,
               static_cast<unsigned long>(picture.table_bytes()),
               static_cast<unsigned long>(info.desc_table_bytes));
        free(file);
        return 1;
    }

    GcxTarget::relocate_table(reinterpret_cast<uint16_t *>(info.desc_table_base), picture,
                              reinterpret_cast<uint32_t>(file));

    // ENGINE_DESC is a word address inside the hard-wired descriptor page.
    const uint16_t desc_word =
        static_cast<uint16_t>((info.desc_table_base - se::DESC_TABLE_BASE) >> 1);

    printf("view: %s, 320x%u, %lu descriptors; Enter returns\n", path, picture.height,
           static_cast<unsigned long>(picture.desc_count));

    // Drop any type-ahead so only a fresh Enter ends the viewing.
    while (enter_pressed())
    {
    }

    for (;;)
    {
        // ENGINE_DESC is a true 16-bit register: one move.w.
        griffin_vsync_wait();
        ENGINE_DESC = desc_word;
        if (enter_pressed())
        {
            break;
        }
    }

    (void)griffin_video_direct_end();
    free(file);
    return 0;
}
