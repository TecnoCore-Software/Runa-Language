class (A) [
    fun who() [
        return "A"
    ]
]
class (B : A) [
]
b = new B()
printr(b.who())
