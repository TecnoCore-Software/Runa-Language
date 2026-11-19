fun test() [
    x = 0
    loop x < 3 [
        printr(x)
        x = x + 1
    ]
    return
]
test()
printr("after test")
