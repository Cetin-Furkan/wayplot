#include "engine.h"

int main(void) {
    auto ok = engine_init();
    if (!ok) {
        return 1;
    }
    return 0;
}
