class (C) [
    fun inner() [
        return 21
    ]
    fun outer() [
        return self.inner() * 2
    ]
]
c = new C()
printr(c.outer())
