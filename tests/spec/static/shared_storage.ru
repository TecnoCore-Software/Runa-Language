class (C) [
    static count = 0
    fun bump() [
        C.count = C.count + 1
    ]
]
a = new C()
b = new C()
a.bump()
b.bump()
printr(C.count)
