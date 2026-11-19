for i = 1 to 5 [
    printr(i)
]

for i = 1 to 5 [
    if i == 3 [
        continue
    ]
    printr(i)
]

for i = 1 to 10 [
    if i == 4 [
        break
    ]
    printr(i)
]

side = 3
for row = 1 to side [
    for col = 1 to row [
        printr(row * 10 + col)
    ]
]

for i = 5 to 1 [
    printr(i)
]
printr("empty range, no problem")
