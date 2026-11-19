x = 0

loop x < 5 [
    printr(x)
    x = x + 1
]

i = 0
loop true [
    i = i + 1
    if i == 4 [
        break
    ]
    printr(i)
]

n = 0
loop n < 6 [
    n = n + 1
    if n == 2 [
        continue
    ]
    if n == 4 [
        continue
    ]
    printr(n)
]

row = 1
loop row <= 3 [
    col = 1
    loop col <= 2 [
        printr(row * 10 + col)
        col = col + 1
    ]
    row = row + 1
]
