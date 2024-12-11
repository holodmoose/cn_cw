#include "server.h"

int main(void) {
    struct server_settings settings = {4096, "127.0.0.1", 8000,    8,
                                       100,  1 << 15,     1 << 10, "/Users/holod/study/BMSTU/sem7_cn_cw"};
    run_server(&settings);
}
