#include "engine.h"
#include "khoros/gfx/blob.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--write-box") == 0) {
        uint8_t buf[512];
        size_t n = khr_blob_write_box(buf, sizeof(buf));
        if (n == 0) {
            return 1;
        }
        FILE* f = fopen(argv[2], "wb");
        if (f == nullptr || fwrite(buf, 1, n, f) != n) {
            if (f != nullptr) {
                fclose(f);
            }
            return 1;
        }
        fclose(f);
        return 0;
    }
    const char* blob_path = (argc > 1) ? argv[1] : nullptr;
    auto ok = engine_init(blob_path);
    if (!ok) {
        return 1;
    }
    return 0;
}
