cimport native/backend

fun announce(text, times) [
    return backend.repeat(text, times)
]
