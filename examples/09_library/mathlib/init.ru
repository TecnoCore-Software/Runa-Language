
precision = 1

fun sqrt(n) [
    r = 0
    loop r * r <= n [
        r = r + 1
    ]
    return r - 1
]

fun add(a, b) [
    return a + b
]

class (Vector) [
    x = 0
    y = 0
    static count = 0

    constructor(a, b) [
        self.x = a
        self.y = b
        Vector.count = Vector.count + 1
    ]

    fun length2() [
        return self.x * self.x + self.y * self.y
    ]

    static fun make(a, b) [
        return new Vector(a, b)
    ]
]
