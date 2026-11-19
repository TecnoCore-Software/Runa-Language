mtn = 10

if mtn < 5 [
    printr("small")
] elif mtn == 10 [
    printr("ten")
] else [
    printr("other")
]

a = 3

if a == 1 [
    printr("one")
] elif a == 2 [
    printr("two")
] elif a == 3 [
    printr("three")
] else [
    printr("other")
]

b = 99

if b == 1 [
    printr("nope")
] elif b == 2 [
    printr("nope")
] else [
    printr("fallback")
]

printr("done")
