fun build(n) [
    if n == 0 [
        return {}
    ]
    return union({n}, build(n - 1))
]
printr(build(5))
printr(sum(build(5)))
printr(count(build(3)))
