for i = 1 to 3 [
    j = 0
    loop j < 10 [
        j = j + 1
        if j == 2 [
            break
        ]
    ]
    printr(i * 10 + j)
]
