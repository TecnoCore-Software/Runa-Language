fun keep(c) [
    return c
]
a = {4, 5}
b = keep(a)
if a === b [
    printr("identity survives the call")
] else [
    printr("broken")
]
