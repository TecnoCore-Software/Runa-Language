fun repeat(label, times) [
    for i = 1 to times [
        printr(label)
    ]
    return
]
repeat("go", 3)
x = 0
loop x < 2 [
    repeat("tick", 1)
    x = x + 1
]
