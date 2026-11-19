/* greet — a tiny C extension for Runa (examples/10_native).
 *
 * Contract notes:
 *  - cstring parameters are borrowed: valid only during the call.
 *  - cstring returns are copied by the Runa bridge immediately.
 *  - long long maps to Runa numbers (int64).
 */
#include <stdlib.h>
#include <string.h>

const char* greet_shout(const char* text) {
    size_t len = strlen(text);
    char* out = (char*)malloc(len + 2);
    memcpy(out, text, len);
    out[len] = '!';
    out[len + 1] = '\0';
    return out;
}

long long greet_excitement(void) {
    return 11;
}
