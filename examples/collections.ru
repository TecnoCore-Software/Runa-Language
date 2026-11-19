
numbers = {10, 20, 30, 40}
printr(numbers)
printr(sum(numbers))
printr(average(numbers))
printr(average({1, 2}))
printr(min(numbers))
printr(max(numbers))
printr(count(numbers))
printr(product({2, 3, 4}))
printr(contains(numbers, 20))
printr(contains(numbers, 99))
printr(empty(numbers))
printr(empty({}))

if {} == null [
    printr("empty is null")
] else [
    printr("empty is not null")
]

printr({1, 2, 2, 3, 3})

a = {1, 2, 3}
b = {3, 4, 5}
printr(union(a, b))
printr(intersection(a, b))
printr(difference(a, b))

printr(sort(b))
printr(reverse(numbers))
printr(first(numbers))
printr(last(numbers))

if {1, 2, 3} == {3, 2, 1} [
    printr("equal as sets")
]
c1 = {1, 2, 3}
c2 = {1, 2, 3}
c3 = c1
if c1 === c2 [
    printr("identical")
] else [
    printr("equal, but two separate sets")
]
if c1 === c3 [
    printr("aliased, same set")
]

fun total(values) [
    return sum(values)
]
printr(total(numbers))

fun makeNumbers() [
    return {10, 20, 30}
]
printr(makeNumbers())

names = {"Runa", "Tluxe", "Lumo"}
printr(names)
printr(sort(names))
printr(contains(names, "Tluxe"))
printr({true, false})
