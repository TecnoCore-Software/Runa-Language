#include <stdlib.h>
#include <string.h>

const char* notify_repeat(const char* text, int n) {
    size_t len = strlen(text);
    char* out = (char*)malloc(len * n + 1);
    out[0] = '\0';
    for (int i = 0; i < n; ++i) {
        strcat(out, text);
    }
    return out;
}
